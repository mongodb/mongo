/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// JS lexical scanner.

#include "frontend/TokenStream.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"
#include "mozilla/TemplateLib.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <type_traits>
#include <utility>

#include "jsnum.h"

#include "frontend/FrontendContext.h"
#include "frontend/Parser.h"
#include "frontend/ParserAtom.h"
#include "frontend/ReservedWords.h"
#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::ColumnNumberOneOrigin, JS::TaggedColumnNumberOneOrigin
#include "js/ErrorReport.h"   // JSErrorBase
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printf.h"                // JS_smprintf
#include "js/RegExpFlags.h"           // JS::RegExpFlags
#include "js/UniquePtr.h"
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/FrameIter.h"  // js::{,NonBuiltin}FrameIter
#include "vm/JSContext.h"
#include "vm/Realm.h"

using mozilla::AsciiAlphanumericToNumber;
using mozilla::AssertedCast;
using mozilla::DecodeOneUtf8CodePoint;
using mozilla::IsAscii;
using mozilla::IsAsciiAlpha;
using mozilla::IsAsciiDigit;
using mozilla::IsAsciiHexDigit;
using mozilla::IsTrailingUnit;
using mozilla::MakeScopeExit;
using mozilla::Maybe;
using mozilla::PointerRangeSize;
using mozilla::Span;
using mozilla::Utf8Unit;

using JS::ReadOnlyCompileOptions;
using JS::RegExpFlag;
using JS::RegExpFlags;

struct ReservedWordInfo {
  const char* chars;  // C string with reserved word text
  js::frontend::TokenKind tokentype;
};

static const ReservedWordInfo reservedWords[] = {
#define RESERVED_WORD_INFO(word, name, type) {#word, js::frontend::type},
    FOR_EACH_JAVASCRIPT_RESERVED_WORD(RESERVED_WORD_INFO)
#undef RESERVED_WORD_INFO
};

enum class ReservedWordsIndex : size_t {
#define ENTRY_(_1, NAME, _3) NAME,
  FOR_EACH_JAVASCRIPT_RESERVED_WORD(ENTRY_)
#undef ENTRY_
};

// Returns a ReservedWordInfo for the specified characters, or nullptr if the
// string is not a reserved word.
template <typename CharT>
static const ReservedWordInfo* FindReservedWord(const CharT* s, size_t length) {
  MOZ_ASSERT(length != 0);

  size_t i;
  const ReservedWordInfo* rw;
  const char* chars;

#define JSRW_LENGTH() length
#define JSRW_AT(column) s[column]
#define JSRW_GOT_MATCH(index) \
  i = (index);                \
  goto got_match;
#define JSRW_TEST_GUESS(index) \
  i = (index);                 \
  goto test_guess;
#define JSRW_NO_MATCH() goto no_match;
#include "frontend/ReservedWordsGenerated.h"
#undef JSRW_NO_MATCH
#undef JSRW_TEST_GUESS
#undef JSRW_GOT_MATCH
#undef JSRW_AT
#undef JSRW_LENGTH

got_match:
  return &reservedWords[i];

test_guess:
  rw = &reservedWords[i];
  chars = rw->chars;
  do {
    if (*s++ != static_cast<unsigned char>(*chars++)) {
      goto no_match;
    }
  } while (--length != 0);
  return rw;

no_match:
  return nullptr;
}

template <>
MOZ_ALWAYS_INLINE const ReservedWordInfo* FindReservedWord<Utf8Unit>(
    const Utf8Unit* units, size_t length) {
  return FindReservedWord(Utf8AsUnsignedChars(units), length);
}

static const ReservedWordInfo* FindReservedWord(
    const js::frontend::TaggedParserAtomIndex atom) {
  switch (atom.rawData()) {
#define CASE_(_1, NAME, _3)                                           \
  case js::frontend::TaggedParserAtomIndex::WellKnownRawData::NAME(): \
    return &reservedWords[size_t(ReservedWordsIndex::NAME)];
    FOR_EACH_JAVASCRIPT_RESERVED_WORD(CASE_)
#undef CASE_
  }

  return nullptr;
}

template <typename CharT>
static constexpr bool IsAsciiBinary(CharT c) {
  using UnsignedCharT = std::make_unsigned_t<CharT>;
  auto uc = static_cast<UnsignedCharT>(c);
  return uc == '0' || uc == '1';
}

template <typename CharT>
static constexpr bool IsAsciiOctal(CharT c) {
  using UnsignedCharT = std::make_unsigned_t<CharT>;
  auto uc = static_cast<UnsignedCharT>(c);
  return '0' <= uc && uc <= '7';
}

template <typename CharT>
static constexpr uint8_t AsciiOctalToNumber(CharT c) {
  using UnsignedCharT = std::make_unsigned_t<CharT>;
  auto uc = static_cast<UnsignedCharT>(c);
  return uc - '0';
}

namespace js {

namespace frontend {

bool IsKeyword(TaggedParserAtomIndex atom) {
  if (const ReservedWordInfo* rw = FindReservedWord(atom)) {
    return TokenKindIsKeyword(rw->tokentype);
  }

  return false;
}

TokenKind ReservedWordTokenKind(TaggedParserAtomIndex name) {
  if (const ReservedWordInfo* rw = FindReservedWord(name)) {
    return rw->tokentype;
  }

  return TokenKind::Limit;
}

const char* ReservedWordToCharZ(TaggedParserAtomIndex name) {
  if (const ReservedWordInfo* rw = FindReservedWord(name)) {
    return ReservedWordToCharZ(rw->tokentype);
  }

  return nullptr;
}

const char* ReservedWordToCharZ(TokenKind tt) {
  MOZ_ASSERT(tt != TokenKind::Name);
  switch (tt) {
#define EMIT_CASE(word, name, type) \
  case type:                        \
    return #word;
    FOR_EACH_JAVASCRIPT_RESERVED_WORD(EMIT_CASE)
#undef EMIT_CASE
    default:
      MOZ_ASSERT_UNREACHABLE("Not a reserved word PropertyName.");
  }
  return nullptr;
}

TaggedParserAtomIndex TokenStreamAnyChars::reservedWordToPropertyName(
    TokenKind tt) const {
  MOZ_ASSERT(tt != TokenKind::Name);
  switch (tt) {
#define EMIT_CASE(word, name, type) \
  case type:                        \
    return TaggedParserAtomIndex::WellKnown::name();
    FOR_EACH_JAVASCRIPT_RESERVED_WORD(EMIT_CASE)
#undef EMIT_CASE
    default:
      MOZ_ASSERT_UNREACHABLE("Not a reserved word TokenKind.");
  }
  return TaggedParserAtomIndex::null();
}

SourceCoords::SourceCoords(FrontendContext* fc, uint32_t initialLineNumber,
                           uint32_t initialOffset)
    : lineStartOffsets_(fc), initialLineNum_(initialLineNumber), lastIndex_(0) {
  // This is actually necessary!  Removing it causes compile errors on
  // GCC and clang.  You could try declaring this:
  //
  //   const uint32_t SourceCoords::MAX_PTR;
  //
  // which fixes the GCC/clang error, but causes bustage on Windows.  Sigh.
  //
  uint32_t maxPtr = MAX_PTR;

  // The first line begins at buffer offset |initialOffset|.  MAX_PTR is the
  // sentinel.  The appends cannot fail because |lineStartOffsets_| has
  // statically-allocated elements.
  MOZ_ASSERT(lineStartOffsets_.capacity() >= 2);
  MOZ_ALWAYS_TRUE(lineStartOffsets_.reserve(2));
  lineStartOffsets_.infallibleAppend(initialOffset);
  lineStartOffsets_.infallibleAppend(maxPtr);
}

MOZ_ALWAYS_INLINE bool SourceCoords::add(uint32_t lineNum,
                                         uint32_t lineStartOffset) {
  uint32_t index = indexFromLineNumber(lineNum);
  uint32_t sentinelIndex = lineStartOffsets_.length() - 1;

  MOZ_ASSERT(lineStartOffsets_[0] <= lineStartOffset);
  MOZ_ASSERT(lineStartOffsets_[sentinelIndex] == MAX_PTR);

  if (index == sentinelIndex) {
    // We haven't seen this newline before.  Update lineStartOffsets_
    // only if lineStartOffsets_.append succeeds, to keep sentinel.
    // Otherwise return false to tell TokenStream about OOM.
    uint32_t maxPtr = MAX_PTR;
    if (!lineStartOffsets_.append(maxPtr)) {
      static_assert(std::is_same_v<decltype(lineStartOffsets_.allocPolicy()),
                                   TempAllocPolicy&>,
                    "this function's caller depends on it reporting an "
                    "error on failure, as TempAllocPolicy ensures");
      return false;
    }

    lineStartOffsets_[index] = lineStartOffset;
  } else {
    // We have seen this newline before (and ungot it).  Do nothing (other
    // than checking it hasn't mysteriously changed).
    // This path can be executed after hitting OOM, so check index.
    MOZ_ASSERT_IF(index < sentinelIndex,
                  lineStartOffsets_[index] == lineStartOffset);
  }
  return true;
}

MOZ_ALWAYS_INLINE bool SourceCoords::fill(const SourceCoords& other) {
  MOZ_ASSERT(lineStartOffsets_[0] == other.lineStartOffsets_[0]);
  MOZ_ASSERT(lineStartOffsets_.back() == MAX_PTR);
  MOZ_ASSERT(other.lineStartOffsets_.back() == MAX_PTR);

  if (lineStartOffsets_.length() >= other.lineStartOffsets_.length()) {
    return true;
  }

  uint32_t sentinelIndex = lineStartOffsets_.length() - 1;
  lineStartOffsets_[sentinelIndex] = other.lineStartOffsets_[sentinelIndex];

  for (size_t i = sentinelIndex + 1; i < other.lineStartOffsets_.length();
       i++) {
    if (!lineStartOffsets_.append(other.lineStartOffsets_[i])) {
      return false;
    }
  }
  return true;
}

MOZ_ALWAYS_INLINE uint32_t
SourceCoords::indexFromOffset(uint32_t offset) const {
  uint32_t iMin, iMax, iMid;

  if (lineStartOffsets_[lastIndex_] <= offset) {
    // If we reach here, offset is on a line the same as or higher than
    // last time.  Check first for the +0, +1, +2 cases, because they
    // typically cover 85--98% of cases.
    if (offset < lineStartOffsets_[lastIndex_ + 1]) {
      return lastIndex_;  // index is same as last time
    }

    // If we reach here, there must be at least one more entry (plus the
    // sentinel).  Try it.
    lastIndex_++;
    if (offset < lineStartOffsets_[lastIndex_ + 1]) {
      return lastIndex_;  // index is one higher than last time
    }

    // The same logic applies here.
    lastIndex_++;
    if (offset < lineStartOffsets_[lastIndex_ + 1]) {
      return lastIndex_;  // index is two higher than last time
    }

    // No luck.  Oh well, we have a better-than-default starting point for
    // the binary search.
    iMin = lastIndex_ + 1;
    MOZ_ASSERT(iMin <
               lineStartOffsets_.length() - 1);  // -1 due to the sentinel

  } else {
    iMin = 0;
  }

  // This is a binary search with deferred detection of equality, which was
  // marginally faster in this case than a standard binary search.
  // The -2 is because |lineStartOffsets_.length() - 1| is the sentinel, and we
  // want one before that.
  iMax = lineStartOffsets_.length() - 2;
  while (iMax > iMin) {
    iMid = iMin + (iMax - iMin) / 2;
    if (offset >= lineStartOffsets_[iMid + 1]) {
      iMin = iMid + 1;  // offset is above lineStartOffsets_[iMid]
    } else {
      iMax = iMid;  // offset is below or within lineStartOffsets_[iMid]
    }
  }

  MOZ_ASSERT(iMax == iMin);
  MOZ_ASSERT(lineStartOffsets_[iMin] <= offset);
  MOZ_ASSERT(offset < lineStartOffsets_[iMin + 1]);

  lastIndex_ = iMin;
  return iMin;
}

SourceCoords::LineToken SourceCoords::lineToken(uint32_t offset) const {
  return LineToken(indexFromOffset(offset), offset);
}

TokenStreamAnyChars::TokenStreamAnyChars(FrontendContext* fc,
                                         const ReadOnlyCompileOptions& options,
                                         StrictModeGetter* smg)
    : fc(fc),
      options_(options),
      strictModeGetter_(smg),
      filename_(options.filename()),
      longLineColumnInfo_(fc),
      srcCoords(fc, options.lineno, options.scriptSourceOffset),
      lineno(options.lineno),
      mutedErrors(options.mutedErrors()) {
  // |isExprEnding| was initially zeroed: overwrite the true entries here.
  isExprEnding[size_t(TokenKind::Comma)] = true;
  isExprEnding[size_t(TokenKind::Semi)] = true;
  isExprEnding[size_t(TokenKind::Colon)] = true;
  isExprEnding[size_t(TokenKind::RightParen)] = true;
  isExprEnding[size_t(TokenKind::RightBracket)] = true;
  isExprEnding[size_t(TokenKind::RightCurly)] = true;
}

template <typename Unit>
TokenStreamCharsBase<Unit>::TokenStreamCharsBase(FrontendContext* fc,
                                                 ParserAtomsTable* parserAtoms,
                                                 const Unit* units,
                                                 size_t length,
                                                 size_t startOffset)
    : TokenStreamCharsShared(fc, parserAtoms),
      sourceUnits(units, length, startOffset) {}

bool FillCharBufferFromSourceNormalizingAsciiLineBreaks(CharBuffer& charBuffer,
                                                        const char16_t* cur,
                                                        const char16_t* end) {
  MOZ_ASSERT(charBuffer.length() == 0);

  while (cur < end) {
    char16_t ch = *cur++;
    if (ch == '\r') {
      ch = '\n';
      if (cur < end && *cur == '\n') {
        cur++;
      }
    }

    if (!charBuffer.append(ch)) {
      return false;
    }
  }

  MOZ_ASSERT(cur == end);
  return true;
}

bool FillCharBufferFromSourceNormalizingAsciiLineBreaks(CharBuffer& charBuffer,
                                                        const Utf8Unit* cur,
                                                        const Utf8Unit* end) {
  MOZ_ASSERT(charBuffer.length() == 0);

  while (cur < end) {
    Utf8Unit unit = *cur++;
    if (MOZ_LIKELY(IsAscii(unit))) {
      char16_t ch = unit.toUint8();
      if (ch == '\r') {
        ch = '\n';
        if (cur < end && *cur == Utf8Unit('\n')) {
          cur++;
        }
      }

      if (!charBuffer.append(ch)) {
        return false;
      }

      continue;
    }

    Maybe<char32_t> ch = DecodeOneUtf8CodePoint(unit, &cur, end);
    MOZ_ASSERT(ch.isSome(),
               "provided source text should already have been validated");

    if (!AppendCodePointToCharBuffer(charBuffer, ch.value())) {
      return false;
    }
  }

  MOZ_ASSERT(cur == end);
  return true;
}

template <typename Unit, class AnyCharsAccess>
TokenStreamSpecific<Unit, AnyCharsAccess>::TokenStreamSpecific(
    FrontendContext* fc, ParserAtomsTable* parserAtoms,
    const ReadOnlyCompileOptions& options, const Unit* units, size_t length)
    : TokenStreamChars<Unit, AnyCharsAccess>(fc, parserAtoms, units, length,
                                             options.scriptSourceOffset) {}

bool TokenStreamAnyChars::checkOptions() {
  // Constrain starting columns to where they will saturate.
  if (options().column.oneOriginValue() >
      JS::LimitedColumnNumberOneOrigin::Limit) {
    reportErrorNoOffset(JSMSG_BAD_COLUMN_NUMBER);
    return false;
  }

  return true;
}

void TokenStreamAnyChars::reportErrorNoOffset(unsigned errorNumber, ...) const {
  va_list args;
  va_start(args, errorNumber);

  reportErrorNoOffsetVA(errorNumber, &args);

  va_end(args);
}

void TokenStreamAnyChars::reportErrorNoOffsetVA(unsigned errorNumber,
                                                va_list* args) const {
  ErrorMetadata metadata;
  computeErrorMetadataNoOffset(&metadata);

  ReportCompileErrorLatin1VA(fc, std::move(metadata), nullptr, errorNumber,
                             args);
}

[[nodiscard]] MOZ_ALWAYS_INLINE bool
TokenStreamAnyChars::internalUpdateLineInfoForEOL(uint32_t lineStartOffset) {
  prevLinebase = linebase;
  linebase = lineStartOffset;
  lineno++;

  // On overflow, report error.
  if (MOZ_UNLIKELY(!lineno)) {
    reportErrorNoOffset(JSMSG_BAD_LINE_NUMBER);
    return false;
  }

  return srcCoords.add(lineno, linebase);
}

#ifdef DEBUG

template <>
inline void SourceUnits<char16_t>::assertNextCodePoint(
    const PeekedCodePoint<char16_t>& peeked) {
  char32_t c = peeked.codePoint();
  if (c < unicode::NonBMPMin) {
    MOZ_ASSERT(peeked.lengthInUnits() == 1);
    MOZ_ASSERT(ptr[0] == c);
  } else {
    MOZ_ASSERT(peeked.lengthInUnits() == 2);
    char16_t lead, trail;
    unicode::UTF16Encode(c, &lead, &trail);
    MOZ_ASSERT(ptr[0] == lead);
    MOZ_ASSERT(ptr[1] == trail);
  }
}

template <>
inline void SourceUnits<Utf8Unit>::assertNextCodePoint(
    const PeekedCodePoint<Utf8Unit>& peeked) {
  char32_t c = peeked.codePoint();

  // This is all roughly indulgence of paranoia only for assertions, so the
  // reimplementation of UTF-8 encoding a code point is (we think) a virtue.
  uint8_t expectedUnits[4] = {};
  if (c < 0x80) {
    expectedUnits[0] = AssertedCast<uint8_t>(c);
  } else if (c < 0x800) {
    expectedUnits[0] = 0b1100'0000 | (c >> 6);
    expectedUnits[1] = 0b1000'0000 | (c & 0b11'1111);
  } else if (c < 0x10000) {
    expectedUnits[0] = 0b1110'0000 | (c >> 12);
    expectedUnits[1] = 0b1000'0000 | ((c >> 6) & 0b11'1111);
    expectedUnits[2] = 0b1000'0000 | (c & 0b11'1111);
  } else {
    expectedUnits[0] = 0b1111'0000 | (c >> 18);
    expectedUnits[1] = 0b1000'0000 | ((c >> 12) & 0b11'1111);
    expectedUnits[2] = 0b1000'0000 | ((c >> 6) & 0b11'1111);
    expectedUnits[3] = 0b1000'0000 | (c & 0b11'1111);
  }

  MOZ_ASSERT(peeked.lengthInUnits() <= 4);
  for (uint8_t i = 0; i < peeked.lengthInUnits(); i++) {
    MOZ_ASSERT(expectedUnits[i] == ptr[i].toUint8());
  }
}

#endif  // DEBUG

static MOZ_ALWAYS_INLINE void RetractPointerToCodePointBoundary(
    const Utf8Unit** ptr, const Utf8Unit* limit) {
  MOZ_ASSERT(*ptr <= limit);

  // |limit| is a code point boundary.
  if (MOZ_UNLIKELY(*ptr == limit)) {
    return;
  }

  // Otherwise rewind past trailing units to the start of the code point.
#ifdef DEBUG
  size_t retracted = 0;
#endif
  while (MOZ_UNLIKELY(IsTrailingUnit((*ptr)[0]))) {
    --*ptr;
#ifdef DEBUG
    retracted++;
#endif
  }

  MOZ_ASSERT(retracted < 4,
             "the longest UTF-8 code point is four units, so this should never "
             "retract more than three units");
}

static MOZ_ALWAYS_INLINE void RetractPointerToCodePointBoundary(
    const char16_t** ptr, const char16_t* limit) {
  MOZ_ASSERT(*ptr <= limit);

  // |limit| is a code point boundary.
  if (MOZ_UNLIKELY(*ptr == limit)) {
    return;
  }

  // Otherwise the pointer must be retracted by one iff it splits a two-unit
  // code point.
  if (MOZ_UNLIKELY(unicode::IsTrailSurrogate((*ptr)[0]))) {
    // Outside test suites testing garbage WTF-16, it's basically guaranteed
    // here that |(*ptr)[-1] (*ptr)[0]| is a surrogate pair.
    if (MOZ_LIKELY(unicode::IsLeadSurrogate((*ptr)[-1]))) {
      --*ptr;
    }
  }
}

template <typename Unit>
JS::ColumnNumberUnsignedOffset TokenStreamAnyChars::computeColumnOffset(
    const LineToken lineToken, const uint32_t offset,
    const SourceUnits<Unit>& sourceUnits) const {
  lineToken.assertConsistentOffset(offset);

  const uint32_t start = srcCoords.lineStart(lineToken);
  const uint32_t offsetInLine = offset - start;

  if constexpr (std::is_same_v<Unit, char16_t>) {
    // Column offset is in UTF-16 code units.
    return JS::ColumnNumberUnsignedOffset(offsetInLine);
  }

  return computeColumnOffsetForUTF8(lineToken, offset, start, offsetInLine,
                                    sourceUnits);
}

template <typename Unit>
JS::ColumnNumberUnsignedOffset TokenStreamAnyChars::computeColumnOffsetForUTF8(
    const LineToken lineToken, const uint32_t offset, const uint32_t start,
    const uint32_t offsetInLine, const SourceUnits<Unit>& sourceUnits) const {
  const uint32_t line = lineNumber(lineToken);

  // Reset the previous offset/column number offset cache for this line, if the
  // previous lookup wasn't on this line.
  if (line != lineOfLastColumnComputation_) {
    lineOfLastColumnComputation_ = line;
    lastChunkVectorForLine_ = nullptr;
    lastOffsetOfComputedColumn_ = start;
    lastComputedColumnOffset_ = JS::ColumnNumberUnsignedOffset::zero();
  }

  // Compute and return the final column number offset from a partially
  // calculated offset/column number offset, using the last-cached
  // offset/column number offset if they're more optimal.
  auto OffsetFromPartial =
      [this, offset, &sourceUnits](
          uint32_t partialOffset,
          JS::ColumnNumberUnsignedOffset partialColumnOffset,
          UnitsType unitsType) {
        MOZ_ASSERT(partialOffset <= offset);

        // If the last lookup on this line was closer to |offset|, use it.
        if (partialOffset < this->lastOffsetOfComputedColumn_ &&
            this->lastOffsetOfComputedColumn_ <= offset) {
          partialOffset = this->lastOffsetOfComputedColumn_;
          partialColumnOffset = this->lastComputedColumnOffset_;
        }

        const Unit* begin = sourceUnits.codeUnitPtrAt(partialOffset);
        const Unit* end = sourceUnits.codeUnitPtrAt(offset);

        size_t offsetDelta =
            AssertedCast<uint32_t>(PointerRangeSize(begin, end));
        partialOffset += offsetDelta;

        if (unitsType == UnitsType::GuaranteedSingleUnit) {
          MOZ_ASSERT(unicode::CountUTF16CodeUnits(begin, end) == offsetDelta,
                     "guaranteed-single-units also guarantee pointer distance "
                     "equals UTF-16 code unit count");
          partialColumnOffset += JS::ColumnNumberUnsignedOffset(offsetDelta);
        } else {
          partialColumnOffset += JS::ColumnNumberUnsignedOffset(
              AssertedCast<uint32_t>(unicode::CountUTF16CodeUnits(begin, end)));
        }

        this->lastOffsetOfComputedColumn_ = partialOffset;
        this->lastComputedColumnOffset_ = partialColumnOffset;
        return partialColumnOffset;
      };

  // We won't add an entry to |longLineColumnInfo_| for lines where the maximum
  // column has offset less than this value.  The most common (non-minified)
  // long line length is likely 80ch, maybe 100ch, so we use that, rounded up to
  // the next power of two for efficient division/multiplication below.
  static constexpr uint32_t ColumnChunkLength =
      mozilla::tl::RoundUpPow2<100>::value;

  // The index within any associated |Vector<ChunkInfo>| of |offset|'s chunk.
  const uint32_t chunkIndex = offsetInLine / ColumnChunkLength;
  if (chunkIndex == 0) {
    // We don't know from an |offset| in the zeroth chunk that this line is even
    // long.  First-chunk info is mostly useless, anyway -- we have |start|
    // already.  So if we have *easy* access to that zeroth chunk, use it --
    // otherwise just count pessimally.  (This will still benefit from caching
    // the last column/offset for computations for successive offsets, so it's
    // not *always* worst-case.)
    UnitsType unitsType;
    if (lastChunkVectorForLine_ && lastChunkVectorForLine_->length() > 0) {
      MOZ_ASSERT((*lastChunkVectorForLine_)[0].columnOffset() ==
                 JS::ColumnNumberUnsignedOffset::zero());
      unitsType = (*lastChunkVectorForLine_)[0].unitsType();
    } else {
      unitsType = UnitsType::PossiblyMultiUnit;
    }

    return OffsetFromPartial(start, JS::ColumnNumberUnsignedOffset::zero(),
                             unitsType);
  }

  // If this line has no chunk vector yet, insert one in the hash map.  (The
  // required index is allocated and filled further down.)
  if (!lastChunkVectorForLine_) {
    auto ptr = longLineColumnInfo_.lookupForAdd(line);
    if (!ptr) {
      // This could rehash and invalidate a cached vector pointer, but the outer
      // condition means we don't have a cached pointer.
      if (!longLineColumnInfo_.add(ptr, line, Vector<ChunkInfo>(fc))) {
        // In case of OOM, just count columns from the start of the line.
        fc->recoverFromOutOfMemory();
        return OffsetFromPartial(start, JS::ColumnNumberUnsignedOffset::zero(),
                                 UnitsType::PossiblyMultiUnit);
      }
    }

    // Note that adding elements to this vector won't invalidate this pointer.
    lastChunkVectorForLine_ = &ptr->value();
  }

  const Unit* const limit = sourceUnits.codeUnitPtrAt(offset);

  auto RetractedOffsetOfChunk = [
#ifdef DEBUG
                                    this,
#endif
                                    start, limit,
                                    &sourceUnits](uint32_t index) {
    MOZ_ASSERT(index < this->lastChunkVectorForLine_->length());

    uint32_t naiveOffset = start + index * ColumnChunkLength;
    const Unit* naivePtr = sourceUnits.codeUnitPtrAt(naiveOffset);

    const Unit* actualPtr = naivePtr;
    RetractPointerToCodePointBoundary(&actualPtr, limit);

#ifdef DEBUG
    if ((*this->lastChunkVectorForLine_)[index].unitsType() ==
        UnitsType::GuaranteedSingleUnit) {
      MOZ_ASSERT(naivePtr == actualPtr, "miscomputed unitsType value");
    }
#endif

    return naiveOffset - PointerRangeSize(actualPtr, naivePtr);
  };

  uint32_t partialOffset;
  JS::ColumnNumberUnsignedOffset partialColumnOffset;
  UnitsType unitsType;

  auto entriesLen = AssertedCast<uint32_t>(lastChunkVectorForLine_->length());
  if (chunkIndex < entriesLen) {
    // We've computed the chunk |offset| resides in.  Compute the column number
    // from the chunk.
    partialOffset = RetractedOffsetOfChunk(chunkIndex);
    partialColumnOffset = (*lastChunkVectorForLine_)[chunkIndex].columnOffset();

    // This is exact if |chunkIndex| isn't the last chunk.
    unitsType = (*lastChunkVectorForLine_)[chunkIndex].unitsType();

    // Otherwise the last chunk is pessimistically assumed to contain multi-unit
    // code points because we haven't fully examined its contents yet -- they
    // may not have been tokenized yet, they could contain encoding errors, or
    // they might not even exist.
    MOZ_ASSERT_IF(chunkIndex == entriesLen - 1,
                  (*lastChunkVectorForLine_)[chunkIndex].unitsType() ==
                      UnitsType::PossiblyMultiUnit);
  } else {
    // Extend the vector from its last entry or the start of the line.  (This is
    // also a suitable partial start point if we must recover from OOM.)
    if (entriesLen > 0) {
      partialOffset = RetractedOffsetOfChunk(entriesLen - 1);
      partialColumnOffset =
          (*lastChunkVectorForLine_)[entriesLen - 1].columnOffset();
    } else {
      partialOffset = start;
      partialColumnOffset = JS::ColumnNumberUnsignedOffset::zero();
    }

    if (!lastChunkVectorForLine_->reserve(chunkIndex + 1)) {
      // As earlier, just start from the greatest offset/column in case of OOM.
      fc->recoverFromOutOfMemory();
      return OffsetFromPartial(partialOffset, partialColumnOffset,
                               UnitsType::PossiblyMultiUnit);
    }

    // OOM is no longer possible now.  \o/

    // The vector always begins with the column of the line start, i.e. zero,
    // with chunk units pessimally assumed not single-unit.
    if (entriesLen == 0) {
      lastChunkVectorForLine_->infallibleAppend(
          ChunkInfo(JS::ColumnNumberUnsignedOffset::zero(),
                    UnitsType::PossiblyMultiUnit));
      entriesLen++;
    }

    do {
      const Unit* const begin = sourceUnits.codeUnitPtrAt(partialOffset);
      const Unit* chunkLimit = sourceUnits.codeUnitPtrAt(
          start + std::min(entriesLen++ * ColumnChunkLength, offsetInLine));

      MOZ_ASSERT(begin < chunkLimit);
      MOZ_ASSERT(chunkLimit <= limit);

      static_assert(
          ColumnChunkLength > SourceUnitTraits<Unit>::maxUnitsLength - 1,
          "any retraction below is assumed to never underflow to the "
          "preceding chunk, even for the longest code point");

      // Prior tokenizing ensured that [begin, limit) is validly encoded, and
      // |begin < chunkLimit|, so any retraction here can't underflow.
      RetractPointerToCodePointBoundary(&chunkLimit, limit);

      MOZ_ASSERT(begin < chunkLimit);
      MOZ_ASSERT(chunkLimit <= limit);

      size_t numUnits = PointerRangeSize(begin, chunkLimit);
      size_t numUTF16CodeUnits =
          unicode::CountUTF16CodeUnits(begin, chunkLimit);

      // If this chunk (which will become non-final at the end of the loop) is
      // all single-unit code points, annotate the chunk accordingly.
      if (numUnits == numUTF16CodeUnits) {
        lastChunkVectorForLine_->back().guaranteeSingleUnits();
      }

      partialOffset += numUnits;
      partialColumnOffset += JS::ColumnNumberUnsignedOffset(numUTF16CodeUnits);

      lastChunkVectorForLine_->infallibleEmplaceBack(
          partialColumnOffset, UnitsType::PossiblyMultiUnit);
    } while (entriesLen < chunkIndex + 1);

    // We're at a spot in the current final chunk, and final chunks never have
    // complete units information, so be pessimistic.
    unitsType = UnitsType::PossiblyMultiUnit;
  }

  return OffsetFromPartial(partialOffset, partialColumnOffset, unitsType);
}

template <typename Unit, class AnyCharsAccess>
JS::LimitedColumnNumberOneOrigin
GeneralTokenStreamChars<Unit, AnyCharsAccess>::computeColumn(
    LineToken lineToken, uint32_t offset) const {
  lineToken.assertConsistentOffset(offset);

  const TokenStreamAnyChars& anyChars = anyCharsAccess();

  JS::ColumnNumberUnsignedOffset columnOffset =
      anyChars.computeColumnOffset(lineToken, offset, this->sourceUnits);

  if (!lineToken.isFirstLine()) {
    return JS::LimitedColumnNumberOneOrigin::fromUnlimited(
        JS::ColumnNumberOneOrigin() + columnOffset);
  }

  if (1 + columnOffset.value() > JS::LimitedColumnNumberOneOrigin::Limit) {
    return JS::LimitedColumnNumberOneOrigin::limit();
  }

  return JS::LimitedColumnNumberOneOrigin::fromUnlimited(
      (anyChars.options_.column + columnOffset).oneOriginValue());
}

template <typename Unit, class AnyCharsAccess>
void GeneralTokenStreamChars<Unit, AnyCharsAccess>::computeLineAndColumn(
    uint32_t offset, uint32_t* line,
    JS::LimitedColumnNumberOneOrigin* column) const {
  const TokenStreamAnyChars& anyChars = anyCharsAccess();

  auto lineToken = anyChars.lineToken(offset);
  *line = anyChars.lineNumber(lineToken);
  *column = computeColumn(lineToken, offset);
}

template <class AnyCharsAccess>
MOZ_COLD void TokenStreamChars<Utf8Unit, AnyCharsAccess>::internalEncodingError(
    uint8_t relevantUnits, unsigned errorNumber, ...) {
  va_list args;
  va_start(args, errorNumber);

  do {
    size_t offset = this->sourceUnits.offset();

    ErrorMetadata err;

    TokenStreamAnyChars& anyChars = anyCharsAccess();

    bool canAddLineOfContext = fillExceptingContext(&err, offset);
    if (canAddLineOfContext) {
      if (!internalComputeLineOfContext(&err, offset)) {
        break;
      }

      // As this is an encoding error, the computed window-end must be
      // identical to the location of the error -- any further on and the
      // window would contain invalid Unicode.
      MOZ_ASSERT_IF(err.lineOfContext != nullptr,
                    err.lineLength == err.tokenOffset);
    }

    auto notes = MakeUnique<JSErrorNotes>();
    if (!notes) {
      ReportOutOfMemory(anyChars.fc);
      break;
    }

    // The largest encoding of a UTF-8 code point is 4 units.  (Encoding an
    // obsolete 5- or 6-byte code point will complain only about a bad lead
    // code unit.)
    constexpr size_t MaxWidth = sizeof("0xHH 0xHH 0xHH 0xHH");

    MOZ_ASSERT(relevantUnits > 0);

    char badUnitsStr[MaxWidth];
    char* ptr = badUnitsStr;
    while (relevantUnits > 0) {
      byteToString(this->sourceUnits.getCodeUnit().toUint8(), ptr);
      ptr[4] = ' ';

      ptr += 5;
      relevantUnits--;
    }

    ptr[-1] = '\0';

    uint32_t line;
    JS::LimitedColumnNumberOneOrigin column;
    computeLineAndColumn(offset, &line, &column);

    if (!notes->addNoteASCII(anyChars.fc, anyChars.getFilename().c_str(), 0,
                             line, JS::ColumnNumberOneOrigin(column),
                             GetErrorMessage, nullptr, JSMSG_BAD_CODE_UNITS,
                             badUnitsStr)) {
      break;
    }

    ReportCompileErrorLatin1VA(anyChars.fc, std::move(err), std::move(notes),
                               errorNumber, &args);
  } while (false);

  va_end(args);
}

template <class AnyCharsAccess>
MOZ_COLD void TokenStreamChars<Utf8Unit, AnyCharsAccess>::badLeadUnit(
    Utf8Unit lead) {
  uint8_t leadValue = lead.toUint8();

  char leadByteStr[5];
  byteToTerminatedString(leadValue, leadByteStr);

  internalEncodingError(1, JSMSG_BAD_LEADING_UTF8_UNIT, leadByteStr);
}

template <class AnyCharsAccess>
MOZ_COLD void TokenStreamChars<Utf8Unit, AnyCharsAccess>::notEnoughUnits(
    Utf8Unit lead, uint8_t remaining, uint8_t required) {
  uint8_t leadValue = lead.toUint8();

  MOZ_ASSERT(required == 2 || required == 3 || required == 4);
  MOZ_ASSERT(remaining < 4);
  MOZ_ASSERT(remaining < required);

  char leadByteStr[5];
  byteToTerminatedString(leadValue, leadByteStr);

  // |toHexChar| produces the desired decimal numbers for values < 4.
  const char expectedStr[] = {toHexChar(required - 1), '\0'};
  const char actualStr[] = {toHexChar(remaining - 1), '\0'};

  internalEncodingError(remaining, JSMSG_NOT_ENOUGH_CODE_UNITS, leadByteStr,
                        expectedStr, required == 2 ? "" : "s", actualStr,
                        remaining == 2 ? " was" : "s were");
}

template <class AnyCharsAccess>
MOZ_COLD void TokenStreamChars<Utf8Unit, AnyCharsAccess>::badTrailingUnit(
    uint8_t unitsObserved) {
  Utf8Unit badUnit =
      this->sourceUnits.addressOfNextCodeUnit()[unitsObserved - 1];

  char badByteStr[5];
  byteToTerminatedString(badUnit.toUint8(), badByteStr);

  internalEncodingError(unitsObserved, JSMSG_BAD_TRAILING_UTF8_UNIT,
                        badByteStr);
}

template <class AnyCharsAccess>
MOZ_COLD void
TokenStreamChars<Utf8Unit, AnyCharsAccess>::badStructurallyValidCodePoint(
    char32_t codePoint, uint8_t codePointLength, const char* reason) {
  // Construct a string like "0x203D" (including null terminator) to include
  // in the error message.  Write the string end-to-start from end to start
  // of an adequately sized |char| array, shifting least significant nibbles
  // off the number and writing the corresponding hex digits until done, then
  // prefixing with "0x".  |codePointStr| points at the incrementally
  // computed string, within |codePointCharsArray|'s bounds.

  // 0x1F'FFFF is the maximum value that can fit in 3+6+6+6 unconstrained
  // bits in a four-byte UTF-8 code unit sequence.
  constexpr size_t MaxHexSize = sizeof(
      "0x1F"
      "FFFF");  // including '\0'
  char codePointCharsArray[MaxHexSize];

  char* codePointStr = std::end(codePointCharsArray);
  *--codePointStr = '\0';

  // Note that by do-while looping here rather than while-looping, this
  // writes a '0' when |codePoint == 0|.
  do {
    MOZ_ASSERT(codePointCharsArray < codePointStr);
    *--codePointStr = toHexChar(codePoint & 0xF);
    codePoint >>= 4;
  } while (codePoint);

  MOZ_ASSERT(codePointCharsArray + 2 <= codePointStr);
  *--codePointStr = 'x';
  *--codePointStr = '0';

  internalEncodingError(codePointLength, JSMSG_FORBIDDEN_UTF8_CODE_POINT,
                        codePointStr, reason);
}

template <class AnyCharsAccess>
[[nodiscard]] bool
TokenStreamChars<Utf8Unit, AnyCharsAccess>::getNonAsciiCodePointDontNormalize(
    Utf8Unit lead, char32_t* codePoint) {
  auto onBadLeadUnit = [this, &lead]() { this->badLeadUnit(lead); };

  auto onNotEnoughUnits = [this, &lead](uint8_t remaining, uint8_t required) {
    this->notEnoughUnits(lead, remaining, required);
  };

  auto onBadTrailingUnit = [this](uint8_t unitsObserved) {
    this->badTrailingUnit(unitsObserved);
  };

  auto onBadCodePoint = [this](char32_t badCodePoint, uint8_t unitsObserved) {
    this->badCodePoint(badCodePoint, unitsObserved);
  };

  auto onNotShortestForm = [this](char32_t badCodePoint,
                                  uint8_t unitsObserved) {
    this->notShortestForm(badCodePoint, unitsObserved);
  };

  // If a valid code point is decoded, this function call consumes its code
  // units.  If not, it ungets the lead code unit and invokes the right error
  // handler, so on failure we must immediately return false.
  SourceUnitsIterator iter(this->sourceUnits);
  Maybe<char32_t> maybeCodePoint = DecodeOneUtf8CodePointInline(
      lead, &iter, SourceUnitsEnd(), onBadLeadUnit, onNotEnoughUnits,
      onBadTrailingUnit, onBadCodePoint, onNotShortestForm);
  if (maybeCodePoint.isNothing()) {
    return false;
  }

  *codePoint = maybeCodePoint.value();
  return true;
}

template <class AnyCharsAccess>
bool TokenStreamChars<char16_t, AnyCharsAccess>::getNonAsciiCodePoint(
    int32_t lead, char32_t* codePoint) {
  MOZ_ASSERT(lead != EOF);
  MOZ_ASSERT(!isAsciiCodePoint(lead),
             "ASCII code unit/point must be handled separately");
  MOZ_ASSERT(lead == this->sourceUnits.previousCodeUnit(),
             "getNonAsciiCodePoint called incorrectly");

  // The code point is usually |lead|: overwrite later if needed.
  *codePoint = AssertedCast<char32_t>(lead);

  // ECMAScript specifically requires that unpaired UTF-16 surrogates be
  // treated as the corresponding code point and not as an error.  See
  // <https://tc39.github.io/ecma262/#sec-ecmascript-language-types-string-type>.
  // Thus this function does not consider any sequence of 16-bit numbers to
  // be intrinsically in error.

  // Dispense with single-unit code points and lone trailing surrogates.
  if (MOZ_LIKELY(!unicode::IsLeadSurrogate(lead))) {
    if (MOZ_UNLIKELY(lead == unicode::LINE_SEPARATOR ||
                     lead == unicode::PARA_SEPARATOR)) {
      if (!updateLineInfoForEOL()) {
#ifdef DEBUG
        // Assign to a sentinel value to hopefully cause errors.
        *codePoint = std::numeric_limits<char32_t>::max();
#endif
        MOZ_MAKE_MEM_UNDEFINED(codePoint, sizeof(*codePoint));
        return false;
      }

      *codePoint = '\n';
    } else {
      MOZ_ASSERT(!IsLineTerminator(*codePoint));
    }

    return true;
  }

  // Also handle a lead surrogate not paired with a trailing surrogate.
  if (MOZ_UNLIKELY(
          this->sourceUnits.atEnd() ||
          !unicode::IsTrailSurrogate(this->sourceUnits.peekCodeUnit()))) {
    MOZ_ASSERT(!IsLineTerminator(*codePoint));
    return true;
  }

  // Otherwise we have a multi-unit code point.
  *codePoint = unicode::UTF16Decode(lead, this->sourceUnits.getCodeUnit());
  MOZ_ASSERT(!IsLineTerminator(*codePoint));
  return true;
}

template <class AnyCharsAccess>
bool TokenStreamChars<Utf8Unit, AnyCharsAccess>::getNonAsciiCodePoint(
    int32_t unit, char32_t* codePoint) {
  MOZ_ASSERT(unit != EOF);
  MOZ_ASSERT(!isAsciiCodePoint(unit),
             "ASCII code unit/point must be handled separately");

  Utf8Unit lead = Utf8Unit(static_cast<unsigned char>(unit));
  MOZ_ASSERT(lead == this->sourceUnits.previousCodeUnit(),
             "getNonAsciiCodePoint called incorrectly");

  auto onBadLeadUnit = [this, &lead]() { this->badLeadUnit(lead); };

  auto onNotEnoughUnits = [this, &lead](uint_fast8_t remaining,
                                        uint_fast8_t required) {
    this->notEnoughUnits(lead, remaining, required);
  };

  auto onBadTrailingUnit = [this](uint_fast8_t unitsObserved) {
    this->badTrailingUnit(unitsObserved);
  };

  auto onBadCodePoint = [this](char32_t badCodePoint,
                               uint_fast8_t unitsObserved) {
    this->badCodePoint(badCodePoint, unitsObserved);
  };

  auto onNotShortestForm = [this](char32_t badCodePoint,
                                  uint_fast8_t unitsObserved) {
    this->notShortestForm(badCodePoint, unitsObserved);
  };

  // This consumes the full, valid code point or ungets |lead| and calls the
  // appropriate error functor on failure.
  SourceUnitsIterator iter(this->sourceUnits);
  Maybe<char32_t> maybeCodePoint = DecodeOneUtf8CodePoint(
      lead, &iter, SourceUnitsEnd(), onBadLeadUnit, onNotEnoughUnits,
      onBadTrailingUnit, onBadCodePoint, onNotShortestForm);
  if (maybeCodePoint.isNothing()) {
    return false;
  }

  char32_t cp = maybeCodePoint.value();
  if (MOZ_UNLIKELY(cp == unicode::LINE_SEPARATOR ||
                   cp == unicode::PARA_SEPARATOR)) {
    if (!updateLineInfoForEOL()) {
#ifdef DEBUG
      // Assign to a sentinel value to hopefully cause errors.
      *codePoint = std::numeric_limits<char32_t>::max();
#endif
      MOZ_MAKE_MEM_UNDEFINED(codePoint, sizeof(*codePoint));
      return false;
    }

    *codePoint = '\n';
  } else {
    MOZ_ASSERT(!IsLineTerminator(cp));
    *codePoint = cp;
  }

  return true;
}

template <>
size_t SourceUnits<char16_t>::findWindowStart(size_t offset) const {
  // This is JS's understanding of UTF-16 that allows lone surrogates, so
  // we have to exclude lone surrogates from [windowStart, offset) ourselves.

  const char16_t* const earliestPossibleStart = codeUnitPtrAt(startOffset_);

  const char16_t* const initial = codeUnitPtrAt(offset);
  const char16_t* p = initial;

  auto HalfWindowSize = [&p, &initial]() {
    return PointerRangeSize(p, initial);
  };

  while (true) {
    MOZ_ASSERT(earliestPossibleStart <= p);
    MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
    if (p <= earliestPossibleStart || HalfWindowSize() >= WindowRadius) {
      break;
    }

    char16_t c = p[-1];

    // This stops at U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR in
    // string and template literals.  These code points do affect line and
    // column coordinates, even as they encode their literal values.
    if (IsLineTerminator(c)) {
      break;
    }

    // Don't allow invalid UTF-16 in pre-context.  (Current users don't
    // require this, and this behavior isn't currently imposed on
    // pre-context, but these facts might change someday.)

    if (MOZ_UNLIKELY(unicode::IsLeadSurrogate(c))) {
      break;
    }

    // Optimistically include the code unit, reverting below if needed.
    p--;

    // If it's not a surrogate at all, keep going.
    if (MOZ_LIKELY(!unicode::IsTrailSurrogate(c))) {
      continue;
    }

    // Stop if we don't have a usable surrogate pair.
    if (HalfWindowSize() >= WindowRadius ||
        p <= earliestPossibleStart ||      // trail surrogate at low end
        !unicode::IsLeadSurrogate(p[-1]))  // no paired lead surrogate
    {
      p++;
      break;
    }

    p--;
  }

  MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
  return offset - HalfWindowSize();
}

template <>
size_t SourceUnits<Utf8Unit>::findWindowStart(size_t offset) const {
  // |offset| must be the location of the error or somewhere before it, so we
  // know preceding data is valid UTF-8.

  const Utf8Unit* const earliestPossibleStart = codeUnitPtrAt(startOffset_);

  const Utf8Unit* const initial = codeUnitPtrAt(offset);
  const Utf8Unit* p = initial;

  auto HalfWindowSize = [&p, &initial]() {
    return PointerRangeSize(p, initial);
  };

  while (true) {
    MOZ_ASSERT(earliestPossibleStart <= p);
    MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
    if (p <= earliestPossibleStart || HalfWindowSize() >= WindowRadius) {
      break;
    }

    // Peek backward for a line break, and only decrement if there is none.
    uint8_t prev = p[-1].toUint8();

    // First check for the ASCII LineTerminators.
    if (prev == '\r' || prev == '\n') {
      break;
    }

    // Now check for the non-ASCII LineTerminators U+2028 LINE SEPARATOR
    // (0xE2 0x80 0xA8) and U+2029 PARAGRAPH (0xE2 0x80 0xA9).  If there
    // aren't three code units available, some comparison here will fail
    // before we'd underflow.
    if (MOZ_UNLIKELY((prev == 0xA8 || prev == 0xA9) &&
                     p[-2].toUint8() == 0x80 && p[-3].toUint8() == 0xE2)) {
      break;
    }

    // Rewind over the non-LineTerminator.  This can't underflow
    // |earliestPossibleStart| because it begins a code point.
    while (IsTrailingUnit(*--p)) {
      continue;
    }

    MOZ_ASSERT(earliestPossibleStart <= p);

    // But if we underflowed |WindowRadius|, adjust forward and stop.
    if (HalfWindowSize() > WindowRadius) {
      static_assert(WindowRadius > 3,
                    "skipping over non-lead code units below must not "
                    "advance past |offset|");

      while (IsTrailingUnit(*++p)) {
        continue;
      }

      MOZ_ASSERT(HalfWindowSize() < WindowRadius);
      break;
    }
  }

  MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
  return offset - HalfWindowSize();
}

template <>
size_t SourceUnits<char16_t>::findWindowEnd(size_t offset) const {
  const char16_t* const initial = codeUnitPtrAt(offset);
  const char16_t* p = initial;

  auto HalfWindowSize = [&initial, &p]() {
    return PointerRangeSize(initial, p);
  };

  while (true) {
    MOZ_ASSERT(p <= limit_);
    MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
    if (p >= limit_ || HalfWindowSize() >= WindowRadius) {
      break;
    }

    char16_t c = *p;

    // This stops at U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR in
    // string and template literals.  These code points do affect line and
    // column coordinates, even as they encode their literal values.
    if (IsLineTerminator(c)) {
      break;
    }

    // Don't allow invalid UTF-16 in post-context.  (Current users don't
    // require this, and this behavior isn't currently imposed on
    // pre-context, but these facts might change someday.)

    if (MOZ_UNLIKELY(unicode::IsTrailSurrogate(c))) {
      break;
    }

    // Optimistically consume the code unit, ungetting it below if needed.
    p++;

    // If it's not a surrogate at all, keep going.
    if (MOZ_LIKELY(!unicode::IsLeadSurrogate(c))) {
      continue;
    }

    // Retract if the lead surrogate would stand alone at the end of the
    // window.
    if (HalfWindowSize() >= WindowRadius ||  // split pair
        p >= limit_ ||                       // half-pair at end of source
        !unicode::IsTrailSurrogate(*p))      // no paired trail surrogate
    {
      p--;
      break;
    }

    p++;
  }

  return offset + HalfWindowSize();
}

template <>
size_t SourceUnits<Utf8Unit>::findWindowEnd(size_t offset) const {
  const Utf8Unit* const initial = codeUnitPtrAt(offset);
  const Utf8Unit* p = initial;

  auto HalfWindowSize = [&initial, &p]() {
    return PointerRangeSize(initial, p);
  };

  while (true) {
    MOZ_ASSERT(p <= limit_);
    MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
    if (p >= limit_ || HalfWindowSize() >= WindowRadius) {
      break;
    }

    // A non-encoding error might be followed by an encoding error within
    // |maxEnd|, so we must validate as we go to not include invalid UTF-8
    // in the computed window.  What joy!

    Utf8Unit lead = *p;
    if (mozilla::IsAscii(lead)) {
      if (IsSingleUnitLineTerminator(lead)) {
        break;
      }

      p++;
      continue;
    }

    PeekedCodePoint<Utf8Unit> peeked = PeekCodePoint(p, limit_);
    if (peeked.isNone()) {
      break;  // encoding error
    }

    char32_t c = peeked.codePoint();
    if (MOZ_UNLIKELY(c == unicode::LINE_SEPARATOR ||
                     c == unicode::PARA_SEPARATOR)) {
      break;
    }

    MOZ_ASSERT(!IsLineTerminator(c));

    uint8_t len = peeked.lengthInUnits();
    if (HalfWindowSize() + len > WindowRadius) {
      break;
    }

    p += len;
  }

  MOZ_ASSERT(HalfWindowSize() <= WindowRadius);
  return offset + HalfWindowSize();
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::advance(size_t position) {
  const Unit* end = this->sourceUnits.codeUnitPtrAt(position);
  while (this->sourceUnits.addressOfNextCodeUnit() < end) {
    if (!getCodePoint()) {
      return false;
    }
  }

  TokenStreamAnyChars& anyChars = anyCharsAccess();
  Token* cur = const_cast<Token*>(&anyChars.currentToken());
  cur->pos.begin = this->sourceUnits.offset();
  cur->pos.end = cur->pos.begin;
#ifdef DEBUG
  cur->type = TokenKind::Limit;
#endif
  MOZ_MAKE_MEM_UNDEFINED(&cur->type, sizeof(cur->type));
  anyChars.lookahead = 0;
  return true;
}

template <typename Unit, class AnyCharsAccess>
void TokenStreamSpecific<Unit, AnyCharsAccess>::seekTo(const Position& pos) {
  TokenStreamAnyChars& anyChars = anyCharsAccess();

  this->sourceUnits.setAddressOfNextCodeUnit(pos.buf,
                                             /* allowPoisoned = */ true);
  anyChars.flags = pos.flags;
  anyChars.lineno = pos.lineno;
  anyChars.linebase = pos.linebase;
  anyChars.prevLinebase = pos.prevLinebase;
  anyChars.lookahead = pos.lookahead;

  anyChars.tokens[anyChars.cursor()] = pos.currentToken;
  for (unsigned i = 0; i < anyChars.lookahead; i++) {
    anyChars.tokens[anyChars.aheadCursor(1 + i)] = pos.lookaheadTokens[i];
  }
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::seekTo(
    const Position& pos, const TokenStreamAnyChars& other) {
  if (!anyCharsAccess().srcCoords.fill(other.srcCoords)) {
    return false;
  }

  seekTo(pos);
  return true;
}

void TokenStreamAnyChars::computeErrorMetadataNoOffset(
    ErrorMetadata* err) const {
  err->isMuted = mutedErrors;
  err->filename = filename_;
  err->lineNumber = 0;
  err->columnNumber = JS::ColumnNumberOneOrigin();

  MOZ_ASSERT(err->lineOfContext == nullptr);
}

bool TokenStreamAnyChars::fillExceptingContext(ErrorMetadata* err,
                                               uint32_t offset) const {
  err->isMuted = mutedErrors;

  // If this TokenStreamAnyChars doesn't have location information, try to
  // get it from the caller.
  if (!filename_) {
    JSContext* maybeCx = context()->maybeCurrentJSContext();
    if (maybeCx) {
      NonBuiltinFrameIter iter(maybeCx,
                               FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK,
                               maybeCx->realm()->principals());
      if (!iter.done() && iter.filename()) {
        err->filename = JS::ConstUTF8CharsZ(iter.filename());
        JS::TaggedColumnNumberOneOrigin columnNumber;
        err->lineNumber = iter.computeLine(&columnNumber);
        err->columnNumber =
            JS::ColumnNumberOneOrigin(columnNumber.oneOriginValue());
        return false;
      }
    }
  }

  // Otherwise use this TokenStreamAnyChars's location information.
  err->filename = filename_;
  return true;
}

template <>
inline void SourceUnits<char16_t>::computeWindowOffsetAndLength(
    const char16_t* encodedWindow, size_t encodedTokenOffset,
    size_t* utf16TokenOffset, size_t encodedWindowLength,
    size_t* utf16WindowLength) const {
  MOZ_ASSERT_UNREACHABLE("shouldn't need to recompute for UTF-16");
}

template <>
inline void SourceUnits<Utf8Unit>::computeWindowOffsetAndLength(
    const Utf8Unit* encodedWindow, size_t encodedTokenOffset,
    size_t* utf16TokenOffset, size_t encodedWindowLength,
    size_t* utf16WindowLength) const {
  MOZ_ASSERT(encodedTokenOffset <= encodedWindowLength,
             "token offset must be within the window, and the two lambda "
             "calls below presume this ordering of values");

  const Utf8Unit* const encodedWindowEnd = encodedWindow + encodedWindowLength;

  size_t i = 0;
  auto ComputeUtf16Count = [&i, &encodedWindow](const Utf8Unit* limit) {
    while (encodedWindow < limit) {
      Utf8Unit lead = *encodedWindow++;
      if (MOZ_LIKELY(IsAscii(lead))) {
        // ASCII contributes a single UTF-16 code unit.
        i++;
        continue;
      }

      Maybe<char32_t> cp = DecodeOneUtf8CodePoint(lead, &encodedWindow, limit);
      MOZ_ASSERT(cp.isSome(),
                 "computed window should only contain valid UTF-8");

      i += unicode::IsSupplementary(cp.value()) ? 2 : 1;
    }

    return i;
  };

  // Compute the token offset from |i == 0| and the initial |encodedWindow|.
  const Utf8Unit* token = encodedWindow + encodedTokenOffset;
  MOZ_ASSERT(token <= encodedWindowEnd);
  *utf16TokenOffset = ComputeUtf16Count(token);

  // Compute the window length, picking up from |i| and |encodedWindow| that,
  // in general, were modified just above.
  *utf16WindowLength = ComputeUtf16Count(encodedWindowEnd);
}

template <typename Unit>
bool TokenStreamCharsBase<Unit>::addLineOfContext(ErrorMetadata* err,
                                                  uint32_t offset) const {
  // Rename the variable to make meaning clearer: an offset into source units
  // in Unit encoding.
  size_t encodedOffset = offset;

  // These are also offsets into source units in Unit encoding.
  size_t encodedWindowStart = sourceUnits.findWindowStart(encodedOffset);
  size_t encodedWindowEnd = sourceUnits.findWindowEnd(encodedOffset);

  size_t encodedWindowLength = encodedWindowEnd - encodedWindowStart;
  MOZ_ASSERT(encodedWindowLength <= SourceUnits::WindowRadius * 2);

  // Don't add a useless "line" of context when the window ends up empty
  // because of an invalid encoding at the start of a line.
  if (encodedWindowLength == 0) {
    MOZ_ASSERT(err->lineOfContext == nullptr,
               "ErrorMetadata::lineOfContext must be null so we don't "
               "have to set the lineLength/tokenOffset fields");
    return true;
  }

  CharBuffer lineOfContext(fc);

  const Unit* encodedWindow = sourceUnits.codeUnitPtrAt(encodedWindowStart);
  if (!FillCharBufferFromSourceNormalizingAsciiLineBreaks(
          lineOfContext, encodedWindow, encodedWindow + encodedWindowLength)) {
    return false;
  }

  size_t utf16WindowLength = lineOfContext.length();

  // The windowed string is null-terminated.
  if (!lineOfContext.append('\0')) {
    return false;
  }

  err->lineOfContext.reset(lineOfContext.extractOrCopyRawBuffer());
  if (!err->lineOfContext) {
    return false;
  }

  size_t encodedTokenOffset = encodedOffset - encodedWindowStart;

  MOZ_ASSERT(encodedTokenOffset <= encodedWindowLength,
             "token offset must be inside the window");

  // The length in UTF-8 code units of a code point is always greater than or
  // equal to the same code point's length in UTF-16 code points.  ASCII code
  // points are 1 unit in either encoding.  Code points in [U+0080, U+10000)
  // are 2-3 UTF-8 code units to 1 UTF-16 code unit.  And code points in
  // [U+10000, U+10FFFF] are 4 UTF-8 code units to 2 UTF-16 code units.
  //
  // Therefore, if encoded window length equals the length in UTF-16 (this is
  // always the case for Unit=char16_t), the UTF-16 offsets are exactly the
  // encoded offsets.  Otherwise we must convert offset/length from UTF-8 to
  // UTF-16.
  if constexpr (std::is_same_v<Unit, char16_t>) {
    MOZ_ASSERT(utf16WindowLength == encodedWindowLength,
               "UTF-16 to UTF-16 shouldn't change window length");
    err->tokenOffset = encodedTokenOffset;
    err->lineLength = encodedWindowLength;
  } else {
    static_assert(std::is_same_v<Unit, Utf8Unit>, "should only see UTF-8 here");

    bool simple = utf16WindowLength == encodedWindowLength;
#ifdef DEBUG
    auto isAscii = [](Unit u) { return IsAscii(u); };
    MOZ_ASSERT(std::all_of(encodedWindow, encodedWindow + encodedWindowLength,
                           isAscii) == simple,
               "equal window lengths in UTF-8 should correspond only to "
               "wholly-ASCII text");
#endif
    if (simple) {
      err->tokenOffset = encodedTokenOffset;
      err->lineLength = encodedWindowLength;
    } else {
      sourceUnits.computeWindowOffsetAndLength(
          encodedWindow, encodedTokenOffset, &err->tokenOffset,
          encodedWindowLength, &err->lineLength);
    }
  }

  return true;
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::computeErrorMetadata(
    ErrorMetadata* err, const ErrorOffset& errorOffset) const {
  if (errorOffset.is<NoOffset>()) {
    anyCharsAccess().computeErrorMetadataNoOffset(err);
    return true;
  }

  uint32_t offset;
  if (errorOffset.is<uint32_t>()) {
    offset = errorOffset.as<uint32_t>();
  } else {
    offset = this->sourceUnits.offset();
  }

  // This function's return value isn't a success/failure indication: it
  // returns true if this TokenStream can be used to provide a line of
  // context.
  if (fillExceptingContext(err, offset)) {
    // Add a line of context from this TokenStream to help with debugging.
    return internalComputeLineOfContext(err, offset);
  }

  // We can't fill in any more here.
  return true;
}

template <typename Unit, class AnyCharsAccess>
void TokenStreamSpecific<Unit, AnyCharsAccess>::reportIllegalCharacter(
    int32_t cp) {
  UniqueChars display = JS_smprintf("U+%04X", cp);
  if (!display) {
    ReportOutOfMemory(anyCharsAccess().fc);
    return;
  }
  error(JSMSG_ILLEGAL_CHARACTER, display.get());
}

// We have encountered a '\': check for a Unicode escape sequence after it.
// Return the length of the escape sequence and the encoded code point (by
// value) if we found a Unicode escape sequence, and skip all code units
// involed.  Otherwise, return 0 and don't advance along the buffer.
template <typename Unit, class AnyCharsAccess>
uint32_t GeneralTokenStreamChars<Unit, AnyCharsAccess>::matchUnicodeEscape(
    char32_t* codePoint) {
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));

  int32_t unit = getCodeUnit();
  if (unit != 'u') {
    // NOTE: |unit| may be EOF here.
    ungetCodeUnit(unit);
    MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));
    return 0;
  }

  char16_t v;
  unit = getCodeUnit();
  if (IsAsciiHexDigit(unit) && this->sourceUnits.matchHexDigits(3, &v)) {
    *codePoint = (AsciiAlphanumericToNumber(unit) << 12) | v;
    return 5;
  }

  if (unit == '{') {
    return matchExtendedUnicodeEscape(codePoint);
  }

  // NOTE: |unit| may be EOF here, so this ungets either one or two units.
  ungetCodeUnit(unit);
  ungetCodeUnit('u');
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));
  return 0;
}

template <typename Unit, class AnyCharsAccess>
uint32_t
GeneralTokenStreamChars<Unit, AnyCharsAccess>::matchExtendedUnicodeEscape(
    char32_t* codePoint) {
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('{'));

  int32_t unit = getCodeUnit();

  // Skip leading zeroes.
  uint32_t leadingZeroes = 0;
  while (unit == '0') {
    leadingZeroes++;
    unit = getCodeUnit();
  }

  size_t i = 0;
  uint32_t code = 0;
  while (IsAsciiHexDigit(unit) && i < 6) {
    code = (code << 4) | AsciiAlphanumericToNumber(unit);
    unit = getCodeUnit();
    i++;
  }

  uint32_t gotten =
      2 +                  // 'u{'
      leadingZeroes + i +  // significant hexdigits
      (unit != EOF);       // subtract a get if it didn't contribute to length

  if (unit == '}' && (leadingZeroes > 0 || i > 0) &&
      code <= unicode::NonBMPMax) {
    *codePoint = code;
    return gotten;
  }

  this->sourceUnits.unskipCodeUnits(gotten);
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));
  return 0;
}

template <typename Unit, class AnyCharsAccess>
uint32_t
GeneralTokenStreamChars<Unit, AnyCharsAccess>::matchUnicodeEscapeIdStart(
    char32_t* codePoint) {
  uint32_t length = matchUnicodeEscape(codePoint);
  if (MOZ_LIKELY(length > 0)) {
    if (MOZ_LIKELY(unicode::IsIdentifierStart(*codePoint))) {
      return length;
    }

    this->sourceUnits.unskipCodeUnits(length);
  }

  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));
  return 0;
}

template <typename Unit, class AnyCharsAccess>
bool GeneralTokenStreamChars<Unit, AnyCharsAccess>::matchUnicodeEscapeIdent(
    char32_t* codePoint) {
  uint32_t length = matchUnicodeEscape(codePoint);
  if (MOZ_LIKELY(length > 0)) {
    if (MOZ_LIKELY(unicode::IsIdentifierPart(*codePoint))) {
      return true;
    }

    this->sourceUnits.unskipCodeUnits(length);
  }

  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('\\'));
  return false;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool
TokenStreamSpecific<Unit, AnyCharsAccess>::matchIdentifierStart(
    IdentifierEscapes* sawEscape) {
  int32_t unit = getCodeUnit();
  if (unit == EOF) {
    error(JSMSG_MISSING_PRIVATE_NAME);
    return false;
  }

  if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
    if (unicode::IsIdentifierStart(char16_t(unit))) {
      *sawEscape = IdentifierEscapes::None;
      return true;
    }

    if (unit == '\\') {
      char32_t codePoint;
      uint32_t escapeLength = matchUnicodeEscapeIdStart(&codePoint);
      if (escapeLength != 0) {
        *sawEscape = IdentifierEscapes::SawUnicodeEscape;
        return true;
      }

      // We could point "into" a mistyped escape, e.g. for "\u{41H}" we
      // could point at the 'H'.  But we don't do that now, so the code
      // unit after the '\' isn't necessarily bad, so just point at the
      // start of the actually-invalid escape.
      ungetCodeUnit('\\');
      error(JSMSG_BAD_ESCAPE);
      return false;
    }
  }

  // Unget the lead code unit before peeking at the full code point.
  ungetCodeUnit(unit);

  PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
  if (!peeked.isNone() && unicode::IsIdentifierStart(peeked.codePoint())) {
    this->sourceUnits.consumeKnownCodePoint(peeked);

    *sawEscape = IdentifierEscapes::None;
    return true;
  }

  error(JSMSG_MISSING_PRIVATE_NAME);
  return false;
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::getDirectives(
    bool isMultiline, bool shouldWarnDeprecated) {
  // Match directive comments used in debugging, such as "//# sourceURL" and
  // "//# sourceMappingURL". Use of "//@" instead of "//#" is deprecated.
  //
  // To avoid a crashing bug in IE, several JavaScript transpilers wrap single
  // line comments containing a source mapping URL inside a multiline
  // comment. To avoid potentially expensive lookahead and backtracking, we
  // only check for this case if we encounter a '#' code unit.

  bool res = getDisplayURL(isMultiline, shouldWarnDeprecated) &&
             getSourceMappingURL(isMultiline, shouldWarnDeprecated);
  if (!res) {
    badToken();
  }

  return res;
}

[[nodiscard]] bool TokenStreamCharsShared::copyCharBufferTo(
    UniquePtr<char16_t[], JS::FreePolicy>* destination) {
  size_t length = charBuffer.length();

  *destination = fc->getAllocator()->make_pod_array<char16_t>(length + 1);
  if (!*destination) {
    return false;
  }

  std::copy(charBuffer.begin(), charBuffer.end(), destination->get());
  (*destination)[length] = '\0';
  return true;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::getDirective(
    bool isMultiline, bool shouldWarnDeprecated, const char* directive,
    uint8_t directiveLength, const char* errorMsgPragma,
    UniquePtr<char16_t[], JS::FreePolicy>* destination) {
  // Stop if we don't find |directive|.  (Note that |directive| must be
  // ASCII, so there are no tricky encoding issues to consider in matching
  // UTF-8/16-agnostically.)
  if (!this->sourceUnits.matchCodeUnits(directive, directiveLength)) {
    return true;
  }

  if (shouldWarnDeprecated) {
    if (!warning(JSMSG_DEPRECATED_PRAGMA, errorMsgPragma)) {
      return false;
    }
  }

  this->charBuffer.clear();

  do {
    int32_t unit = peekCodeUnit();
    if (unit == EOF) {
      break;
    }

    if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
      if (unicode::IsSpace(AssertedCast<Latin1Char>(unit))) {
        break;
      }

      consumeKnownCodeUnit(unit);

      // Debugging directives can occur in both single- and multi-line
      // comments. If we're currently inside a multi-line comment, we
      // also must recognize multi-line comment terminators.
      if (isMultiline && unit == '*' && peekCodeUnit() == '/') {
        ungetCodeUnit('*');
        break;
      }

      if (!this->charBuffer.append(unit)) {
        return false;
      }

      continue;
    }

    // This ignores encoding errors: subsequent caller-side code to
    // handle the remaining source text in the comment will do so.
    PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
    if (peeked.isNone() || unicode::IsSpace(peeked.codePoint())) {
      break;
    }

    MOZ_ASSERT(!IsLineTerminator(peeked.codePoint()),
               "!IsSpace must imply !IsLineTerminator or else we'll fail to "
               "maintain line-info/flags for EOL");
    this->sourceUnits.consumeKnownCodePoint(peeked);

    if (!AppendCodePointToCharBuffer(this->charBuffer, peeked.codePoint())) {
      return false;
    }
  } while (true);

  if (this->charBuffer.empty()) {
    // The directive's URL was missing, but comments can contain anything,
    // so it isn't an error.
    return true;
  }

  return copyCharBufferTo(destination);
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::getDisplayURL(
    bool isMultiline, bool shouldWarnDeprecated) {
  // Match comments of the form "//# sourceURL=<url>" or
  // "/\* //# sourceURL=<url> *\/"
  //
  // Note that while these are labeled "sourceURL" in the source text,
  // internally we refer to it as a "displayURL" to distinguish what the
  // developer would like to refer to the source as from the source's actual
  // URL.

  static constexpr char sourceURLDirective[] = " sourceURL=";
  constexpr uint8_t sourceURLDirectiveLength = js_strlen(sourceURLDirective);
  return getDirective(isMultiline, shouldWarnDeprecated, sourceURLDirective,
                      sourceURLDirectiveLength, "sourceURL",
                      &anyCharsAccess().displayURL_);
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::getSourceMappingURL(
    bool isMultiline, bool shouldWarnDeprecated) {
  // Match comments of the form "//# sourceMappingURL=<url>" or
  // "/\* //# sourceMappingURL=<url> *\/"

  static constexpr char sourceMappingURLDirective[] = " sourceMappingURL=";
  constexpr uint8_t sourceMappingURLDirectiveLength =
      js_strlen(sourceMappingURLDirective);
  return getDirective(isMultiline, shouldWarnDeprecated,
                      sourceMappingURLDirective,
                      sourceMappingURLDirectiveLength, "sourceMappingURL",
                      &anyCharsAccess().sourceMapURL_);
}

template <typename Unit, class AnyCharsAccess>
MOZ_ALWAYS_INLINE Token*
GeneralTokenStreamChars<Unit, AnyCharsAccess>::newTokenInternal(
    TokenKind kind, TokenStart start, TokenKind* out) {
  MOZ_ASSERT(kind < TokenKind::Limit);
  MOZ_ASSERT(kind != TokenKind::Eol,
             "TokenKind::Eol should never be used in an actual Token, only "
             "returned by peekTokenSameLine()");

  TokenStreamAnyChars& anyChars = anyCharsAccess();
  anyChars.flags.isDirtyLine = true;

  Token* token = anyChars.allocateToken();

  *out = token->type = kind;
  token->pos = TokenPos(start.offset(), this->sourceUnits.offset());
  MOZ_ASSERT(token->pos.begin <= token->pos.end);

  // NOTE: |token->modifier| is set in |newToken()| so that optimized,
  // non-debug code won't do any work to pass a modifier-argument that will
  // never be used.

  return token;
}

template <typename Unit, class AnyCharsAccess>
MOZ_COLD bool GeneralTokenStreamChars<Unit, AnyCharsAccess>::badToken() {
  // We didn't get a token, so don't set |flags.isDirtyLine|.
  anyCharsAccess().flags.hadError = true;

  // Poisoning sourceUnits on error establishes an invariant: once an
  // erroneous token has been seen, sourceUnits will not be consulted again.
  // This is true because the parser will deal with the illegal token by
  // aborting parsing immediately.
  this->sourceUnits.poisonInDebug();

  return false;
};

bool AppendCodePointToCharBuffer(CharBuffer& charBuffer, char32_t codePoint) {
  MOZ_ASSERT(codePoint <= unicode::NonBMPMax,
             "should only be processing code points validly decoded from UTF-8 "
             "or WTF-16 source text (surrogate code points permitted)");

  char16_t units[2];
  unsigned numUnits = 0;
  unicode::UTF16Encode(codePoint, units, &numUnits);

  MOZ_ASSERT(numUnits == 1 || numUnits == 2,
             "UTF-16 code points are only encoded in one or two units");

  if (!charBuffer.append(units[0])) {
    return false;
  }

  if (numUnits == 1) {
    return true;
  }

  return charBuffer.append(units[1]);
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::putIdentInCharBuffer(
    const Unit* identStart) {
  const Unit* const originalAddress = this->sourceUnits.addressOfNextCodeUnit();
  this->sourceUnits.setAddressOfNextCodeUnit(identStart);

  auto restoreNextRawCharAddress = MakeScopeExit([this, originalAddress]() {
    this->sourceUnits.setAddressOfNextCodeUnit(originalAddress);
  });

  this->charBuffer.clear();
  do {
    int32_t unit = getCodeUnit();
    if (unit == EOF) {
      break;
    }

    char32_t codePoint;
    if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
      if (unicode::IsIdentifierPart(char16_t(unit)) || unit == '#') {
        if (!this->charBuffer.append(unit)) {
          return false;
        }

        continue;
      }

      if (unit != '\\' || !matchUnicodeEscapeIdent(&codePoint)) {
        break;
      }
    } else {
      // |restoreNextRawCharAddress| undoes all gets, and this function
      // doesn't update line/column info.
      char32_t cp;
      if (!getNonAsciiCodePointDontNormalize(toUnit(unit), &cp)) {
        return false;
      }

      codePoint = cp;
      if (!unicode::IsIdentifierPart(codePoint)) {
        break;
      }
    }

    if (!AppendCodePointToCharBuffer(this->charBuffer, codePoint)) {
      return false;
    }
  } while (true);

  return true;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::identifierName(
    TokenStart start, const Unit* identStart, IdentifierEscapes escaping,
    Modifier modifier, NameVisibility visibility, TokenKind* out) {
  // Run the bad-token code for every path out of this function except the
  // two success-cases.
  auto noteBadToken = MakeScopeExit([this]() { this->badToken(); });

  // We've already consumed an initial code point in the identifer, to *know*
  // that this is an identifier.  So no need to worry about not consuming any
  // code points in the loop below.
  int32_t unit;
  while (true) {
    unit = peekCodeUnit();
    if (unit == EOF) {
      break;
    }

    if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
      consumeKnownCodeUnit(unit);

      if (MOZ_UNLIKELY(
              !unicode::IsIdentifierPart(static_cast<char16_t>(unit)))) {
        // Handle a Unicode escape -- otherwise it's not part of the
        // identifier.
        char32_t codePoint;
        if (unit != '\\' || !matchUnicodeEscapeIdent(&codePoint)) {
          ungetCodeUnit(unit);
          break;
        }

        escaping = IdentifierEscapes::SawUnicodeEscape;
      }
    } else {
      // This ignores encoding errors: subsequent caller-side code to
      // handle source text after the IdentifierName will do so.
      PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
      if (peeked.isNone() || !unicode::IsIdentifierPart(peeked.codePoint())) {
        break;
      }

      MOZ_ASSERT(!IsLineTerminator(peeked.codePoint()),
                 "IdentifierPart must guarantee !IsLineTerminator or "
                 "else we'll fail to maintain line-info/flags for EOL");

      this->sourceUnits.consumeKnownCodePoint(peeked);
    }
  }

  TaggedParserAtomIndex atom;
  if (MOZ_UNLIKELY(escaping == IdentifierEscapes::SawUnicodeEscape)) {
    // Identifiers containing Unicode escapes have to be converted into
    // tokenbuf before atomizing.
    if (!putIdentInCharBuffer(identStart)) {
      return false;
    }

    atom = drainCharBufferIntoAtom();
  } else {
    // Escape-free identifiers can be created directly from sourceUnits.
    const Unit* chars = identStart;
    size_t length = this->sourceUnits.addressOfNextCodeUnit() - identStart;

    // Private identifiers start with a '#', and so cannot be reserved words.
    if (visibility == NameVisibility::Public) {
      // Represent reserved words lacking escapes as reserved word tokens.
      if (const ReservedWordInfo* rw = FindReservedWord(chars, length)) {
        noteBadToken.release();
        newSimpleToken(rw->tokentype, start, modifier, out);
        return true;
      }
    }

    atom = atomizeSourceChars(Span(chars, length));
  }
  if (!atom) {
    return false;
  }

  noteBadToken.release();
  if (visibility == NameVisibility::Private) {
    newPrivateNameToken(atom, start, modifier, out);
    return true;
  }
  newNameToken(atom, start, modifier, out);
  return true;
}

enum FirstCharKind {
  // A char16_t has the 'OneChar' kind if it, by itself, constitutes a valid
  // token that cannot also be a prefix of a longer token.  E.g. ';' has the
  // OneChar kind, but '+' does not, because '++' and '+=' are valid longer
  // tokens
  // that begin with '+'.
  //
  // The few token kinds satisfying these properties cover roughly 35--45%
  // of the tokens seen in practice.
  //
  // We represent the 'OneChar' kind with any positive value less than
  // TokenKind::Limit.  This representation lets us associate
  // each one-char token char16_t with a TokenKind and thus avoid
  // a subsequent char16_t-to-TokenKind conversion.
  OneChar_Min = 0,
  OneChar_Max = size_t(TokenKind::Limit) - 1,

  Space = size_t(TokenKind::Limit),
  Ident,
  Dec,
  String,
  EOL,
  ZeroDigit,
  Other,

  LastCharKind = Other
};

// OneChar: 40,  41,  44,  58,  59,  91,  93,  123, 125, 126:
//          '(', ')', ',', ':', ';', '[', ']', '{', '}', '~'
// Ident:   36, 65..90, 95, 97..122: '$', 'A'..'Z', '_', 'a'..'z'
// Dot:     46: '.'
// Equals:  61: '='
// String:  34, 39, 96: '"', '\'', '`'
// Dec:     49..57: '1'..'9'
// Plus:    43: '+'
// ZeroDigit:  48: '0'
// Space:   9, 11, 12, 32: '\t', '\v', '\f', ' '
// EOL:     10, 13: '\n', '\r'
//
#define T_COMMA size_t(TokenKind::Comma)
#define T_COLON size_t(TokenKind::Colon)
#define T_BITNOT size_t(TokenKind::BitNot)
#define T_LP size_t(TokenKind::LeftParen)
#define T_RP size_t(TokenKind::RightParen)
#define T_SEMI size_t(TokenKind::Semi)
#define T_LB size_t(TokenKind::LeftBracket)
#define T_RB size_t(TokenKind::RightBracket)
#define T_LC size_t(TokenKind::LeftCurly)
#define T_RC size_t(TokenKind::RightCurly)
#define _______ Other
static const uint8_t firstCharKinds[] = {
    // clang-format off
/*         0        1        2        3        4        5        6        7        8        9    */
/*   0+ */ _______, _______, _______, _______, _______, _______, _______, _______, _______,   Space,
/*  10+ */     EOL,   Space,   Space,     EOL, _______, _______, _______, _______, _______, _______,
/*  20+ */ _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
/*  30+ */ _______, _______,   Space, _______,  String, _______,   Ident, _______, _______,  String,
/*  40+ */    T_LP,    T_RP, _______, _______, T_COMMA, _______, _______, _______,ZeroDigit,    Dec,
/*  50+ */     Dec,     Dec,     Dec,     Dec,     Dec,     Dec,     Dec,     Dec, T_COLON,  T_SEMI,
/*  60+ */ _______, _______, _______, _______, _______,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  70+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  80+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  90+ */   Ident,    T_LB, _______,    T_RB, _______,   Ident,  String,   Ident,   Ident,   Ident,
/* 100+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/* 110+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/* 120+ */   Ident,   Ident,   Ident,    T_LC, _______,    T_RC,T_BITNOT, _______
    // clang-format on
};
#undef T_COMMA
#undef T_COLON
#undef T_BITNOT
#undef T_LP
#undef T_RP
#undef T_SEMI
#undef T_LB
#undef T_RB
#undef T_LC
#undef T_RC
#undef _______

static_assert(LastCharKind < (1 << (sizeof(firstCharKinds[0]) * 8)),
              "Elements of firstCharKinds[] are too small");

template <>
void SourceUnits<char16_t>::consumeRestOfSingleLineComment() {
  while (MOZ_LIKELY(!atEnd())) {
    char16_t unit = peekCodeUnit();
    if (IsLineTerminator(unit)) {
      return;
    }

    consumeKnownCodeUnit(unit);
  }
}

template <>
void SourceUnits<Utf8Unit>::consumeRestOfSingleLineComment() {
  while (MOZ_LIKELY(!atEnd())) {
    const Utf8Unit unit = peekCodeUnit();
    if (IsSingleUnitLineTerminator(unit)) {
      return;
    }

    if (MOZ_LIKELY(IsAscii(unit))) {
      consumeKnownCodeUnit(unit);
      continue;
    }

    PeekedCodePoint<Utf8Unit> peeked = peekCodePoint();
    if (peeked.isNone()) {
      return;
    }

    char32_t c = peeked.codePoint();
    if (MOZ_UNLIKELY(c == unicode::LINE_SEPARATOR ||
                     c == unicode::PARA_SEPARATOR)) {
      return;
    }

    consumeKnownCodePoint(peeked);
  }
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] MOZ_ALWAYS_INLINE bool
TokenStreamSpecific<Unit, AnyCharsAccess>::matchInteger(
    IsIntegerUnit isIntegerUnit, int32_t* nextUnit) {
  int32_t unit = getCodeUnit();
  if (!isIntegerUnit(unit)) {
    *nextUnit = unit;
    return true;
  }
  return matchIntegerAfterFirstDigit(isIntegerUnit, nextUnit);
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] MOZ_ALWAYS_INLINE bool
TokenStreamSpecific<Unit, AnyCharsAccess>::matchIntegerAfterFirstDigit(
    IsIntegerUnit isIntegerUnit, int32_t* nextUnit) {
  int32_t unit;
  while (true) {
    unit = getCodeUnit();
    if (isIntegerUnit(unit)) {
      continue;
    }
    if (unit != '_') {
      break;
    }
    unit = getCodeUnit();
    if (!isIntegerUnit(unit)) {
      if (unit == '_') {
        ungetCodeUnit(unit);
        error(JSMSG_NUMBER_MULTIPLE_ADJACENT_UNDERSCORES);
      } else {
        ungetCodeUnit(unit);
        ungetCodeUnit('_');
        error(JSMSG_NUMBER_END_WITH_UNDERSCORE);
      }
      return false;
    }
  }

  *nextUnit = unit;
  return true;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::decimalNumber(
    int32_t unit, TokenStart start, const Unit* numStart, Modifier modifier,
    TokenKind* out) {
  // Run the bad-token code for every path out of this function except the
  // one success-case.
  auto noteBadToken = MakeScopeExit([this]() { this->badToken(); });

  // Consume integral component digits.
  if (IsAsciiDigit(unit)) {
    if (!matchIntegerAfterFirstDigit(IsAsciiDigit, &unit)) {
      return false;
    }
  }

  // Numbers contain no escapes, so we can read directly from |sourceUnits|.
  double dval;
  bool isBigInt = false;
  DecimalPoint decimalPoint = NoDecimal;
  if (unit != '.' && unit != 'e' && unit != 'E' && unit != 'n') {
    // NOTE: |unit| may be EOF here.
    ungetCodeUnit(unit);

    // Most numbers are pure decimal integers without fractional component
    // or exponential notation.  Handle that with optimized code.
    if (!GetDecimalInteger(numStart, this->sourceUnits.addressOfNextCodeUnit(),
                           &dval)) {
      ReportOutOfMemory(this->fc);
      return false;
    }
  } else if (unit == 'n') {
    isBigInt = true;
    unit = peekCodeUnit();
  } else {
    // Consume any decimal dot and fractional component.
    if (unit == '.') {
      decimalPoint = HasDecimal;
      if (!matchInteger(IsAsciiDigit, &unit)) {
        return false;
      }
    }

    // Consume any exponential notation.
    if (unit == 'e' || unit == 'E') {
      unit = getCodeUnit();
      if (unit == '+' || unit == '-') {
        unit = getCodeUnit();
      }

      // Exponential notation must contain at least one digit.
      if (!IsAsciiDigit(unit)) {
        ungetCodeUnit(unit);
        error(JSMSG_MISSING_EXPONENT);
        return false;
      }

      // Consume exponential digits.
      if (!matchIntegerAfterFirstDigit(IsAsciiDigit, &unit)) {
        return false;
      }
    }

    ungetCodeUnit(unit);

    if (!GetDecimal(numStart, this->sourceUnits.addressOfNextCodeUnit(),
                    &dval)) {
      ReportOutOfMemory(this->fc);
      return false;
    }
  }

  // Number followed by IdentifierStart is an error.  (This is the only place
  // in ECMAScript where token boundary is inadequate to properly separate
  // two tokens, necessitating this unaesthetic lookahead.)
  if (unit != EOF) {
    if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
      if (unicode::IsIdentifierStart(char16_t(unit))) {
        error(JSMSG_IDSTART_AFTER_NUMBER);
        return false;
      }
    } else {
      // This ignores encoding errors: subsequent caller-side code to
      // handle source text after the number will do so.
      PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
      if (!peeked.isNone() && unicode::IsIdentifierStart(peeked.codePoint())) {
        error(JSMSG_IDSTART_AFTER_NUMBER);
        return false;
      }
    }
  }

  noteBadToken.release();

  if (isBigInt) {
    return bigIntLiteral(start, modifier, out);
  }

  newNumberToken(dval, decimalPoint, start, modifier, out);
  return true;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::regexpLiteral(
    TokenStart start, TokenKind* out) {
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == Unit('/'));
  this->charBuffer.clear();

  auto ProcessNonAsciiCodePoint = [this](int32_t lead) {
    MOZ_ASSERT(lead != EOF);
    MOZ_ASSERT(!this->isAsciiCodePoint(lead));

    char32_t codePoint;
    if (!this->getNonAsciiCodePointDontNormalize(this->toUnit(lead),
                                                 &codePoint)) {
      return false;
    }

    if (MOZ_UNLIKELY(codePoint == unicode::LINE_SEPARATOR ||
                     codePoint == unicode::PARA_SEPARATOR)) {
      this->sourceUnits.ungetLineOrParagraphSeparator();
      this->error(JSMSG_UNTERMINATED_REGEXP);
      return false;
    }

    return AppendCodePointToCharBuffer(this->charBuffer, codePoint);
  };

  auto ReportUnterminatedRegExp = [this](int32_t unit) {
    this->ungetCodeUnit(unit);
    this->error(JSMSG_UNTERMINATED_REGEXP);
  };

  bool inCharClass = false;
  do {
    int32_t unit = getCodeUnit();
    if (unit == EOF) {
      ReportUnterminatedRegExp(unit);
      return badToken();
    }

    if (MOZ_UNLIKELY(!isAsciiCodePoint(unit))) {
      if (!ProcessNonAsciiCodePoint(unit)) {
        return badToken();
      }

      continue;
    }

    if (unit == '\\') {
      if (!this->charBuffer.append(unit)) {
        return badToken();
      }

      unit = getCodeUnit();
      if (unit == EOF) {
        ReportUnterminatedRegExp(unit);
        return badToken();
      }

      // Fallthrough only handles ASCII code points, so
      // deal with non-ASCII and skip everything else.
      if (MOZ_UNLIKELY(!isAsciiCodePoint(unit))) {
        if (!ProcessNonAsciiCodePoint(unit)) {
          return badToken();
        }

        continue;
      }
    } else if (unit == '[') {
      inCharClass = true;
    } else if (unit == ']') {
      inCharClass = false;
    } else if (unit == '/' && !inCharClass) {
      // For IE compat, allow unescaped / in char classes.
      break;
    }

    // NOTE: Non-ASCII LineTerminators were handled by
    //       ProcessNonAsciiCodePoint calls above.
    if (unit == '\r' || unit == '\n') {
      ReportUnterminatedRegExp(unit);
      return badToken();
    }

    MOZ_ASSERT(!IsLineTerminator(AssertedCast<char32_t>(unit)));
    if (!this->charBuffer.append(unit)) {
      return badToken();
    }
  } while (true);

  int32_t unit;
  RegExpFlags reflags = RegExpFlag::NoFlags;
  while (true) {
    uint8_t flag;
    unit = getCodeUnit();
    if (unit == 'd') {
      flag = RegExpFlag::HasIndices;
    } else if (unit == 'g') {
      flag = RegExpFlag::Global;
    } else if (unit == 'i') {
      flag = RegExpFlag::IgnoreCase;
    } else if (unit == 'm') {
      flag = RegExpFlag::Multiline;
    } else if (unit == 's') {
      flag = RegExpFlag::DotAll;
    } else if (unit == 'u') {
      flag = RegExpFlag::Unicode;
    } else if (unit == 'v') {
      flag = RegExpFlag::UnicodeSets;
    } else if (unit == 'y') {
      flag = RegExpFlag::Sticky;
    } else if (IsAsciiAlpha(unit)) {
      flag = RegExpFlag::NoFlags;
    } else {
      break;
    }

    if ((reflags & flag) || flag == RegExpFlag::NoFlags) {
      ungetCodeUnit(unit);
      char buf[2] = {char(unit), '\0'};
      error(JSMSG_BAD_REGEXP_FLAG, buf);
      return badToken();
    }

    // /u and /v flags are mutually exclusive.
    if (((reflags & RegExpFlag::Unicode) && (flag & RegExpFlag::UnicodeSets)) ||
        ((reflags & RegExpFlag::UnicodeSets) && (flag & RegExpFlag::Unicode))) {
      ungetCodeUnit(unit);
      char buf[2] = {char(unit), '\0'};
      error(JSMSG_BAD_REGEXP_FLAG, buf);
      return badToken();
    }

    reflags |= flag;
  }
  ungetCodeUnit(unit);

  newRegExpToken(reflags, start, out);
  return true;
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::bigIntLiteral(
    TokenStart start, Modifier modifier, TokenKind* out) {
  MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == toUnit('n'));
  MOZ_ASSERT(this->sourceUnits.offset() > start.offset());
  uint32_t length = this->sourceUnits.offset() - start.offset();
  MOZ_ASSERT(length >= 2);
  this->charBuffer.clear();
  mozilla::Range<const Unit> chars(
      this->sourceUnits.codeUnitPtrAt(start.offset()), length);
  for (uint32_t idx = 0; idx < length - 1; idx++) {
    int32_t unit = CodeUnitValue(chars[idx]);
    // Char buffer may start with a 0[bBoOxX] prefix, then follows with
    // binary, octal, decimal, or hex digits.  Already checked by caller, as
    // the "n" indicating bigint comes at the end.
    MOZ_ASSERT(isAsciiCodePoint(unit));
    // Skip over any separators.
    if (unit == '_') {
      continue;
    }
    if (!AppendCodePointToCharBuffer(this->charBuffer, unit)) {
      return false;
    }
  }
  newBigIntToken(start, modifier, out);
  return true;
}

template <typename Unit, class AnyCharsAccess>
void GeneralTokenStreamChars<Unit,
                             AnyCharsAccess>::consumeOptionalHashbangComment() {
  MOZ_ASSERT(this->sourceUnits.atStart(),
             "HashBangComment can only appear immediately at the start of a "
             "Script or Module");

  // HashbangComment ::
  //   #!  SingleLineCommentChars_opt

  if (!matchCodeUnit('#')) {
    // HashbangComment is optional at start of Script or Module.
    return;
  }

  if (!matchCodeUnit('!')) {
    // # not followed by ! at start of Script or Module is an error, but normal
    // parsing code will handle that error just fine if we let it.
    ungetCodeUnit('#');
    return;
  }

  // This doesn't consume a concluding LineTerminator, and it stops consuming
  // just before any encoding error.  The subsequent |getToken| call will call
  // |getTokenInternal| below which will handle these possibilities.
  this->sourceUnits.consumeRestOfSingleLineComment();
}

template <typename Unit, class AnyCharsAccess>
[[nodiscard]] bool TokenStreamSpecific<Unit, AnyCharsAccess>::getTokenInternal(
    TokenKind* const ttp, const Modifier modifier) {
  // Assume we'll fail: success cases will overwrite this.
#ifdef DEBUG
  *ttp = TokenKind::Limit;
#endif
  MOZ_MAKE_MEM_UNDEFINED(ttp, sizeof(*ttp));

  // This loop runs more than once only when whitespace or comments are
  // encountered.
  do {
    int32_t unit = peekCodeUnit();
    if (MOZ_UNLIKELY(unit == EOF)) {
      MOZ_ASSERT(this->sourceUnits.atEnd());
      anyCharsAccess().flags.isEOF = true;
      TokenStart start(this->sourceUnits, 0);
      newSimpleToken(TokenKind::Eof, start, modifier, ttp);
      return true;
    }

    if (MOZ_UNLIKELY(!isAsciiCodePoint(unit))) {
      // Non-ASCII code points can only be identifiers or whitespace.  It would
      // be nice to compute these *after* discarding whitespace, but IN A WORLD
      // where |unicode::IsSpace| requires consuming a variable number of code
      // units, it's easier to assume it's an identifier and maybe do a little
      // wasted work, than to unget and compute and reget if whitespace.
      TokenStart start(this->sourceUnits, 0);
      const Unit* identStart = this->sourceUnits.addressOfNextCodeUnit();

      PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
      if (peeked.isNone()) {
        MOZ_ALWAYS_FALSE(getCodePoint());
        return badToken();
      }

      char32_t cp = peeked.codePoint();
      if (unicode::IsSpace(cp)) {
        this->sourceUnits.consumeKnownCodePoint(peeked);
        if (IsLineTerminator(cp)) {
          if (!updateLineInfoForEOL()) {
            return badToken();
          }

          anyCharsAccess().updateFlagsForEOL();
        }

        continue;
      }

      static_assert(isAsciiCodePoint('$'),
                    "IdentifierStart contains '$', but as "
                    "!IsUnicodeIDStart('$'), ensure that '$' is never "
                    "handled here");
      static_assert(isAsciiCodePoint('_'),
                    "IdentifierStart contains '_', but as "
                    "!IsUnicodeIDStart('_'), ensure that '_' is never "
                    "handled here");

      if (MOZ_LIKELY(unicode::IsUnicodeIDStart(cp))) {
        this->sourceUnits.consumeKnownCodePoint(peeked);
        MOZ_ASSERT(!IsLineTerminator(cp),
                   "IdentifierStart must guarantee !IsLineTerminator "
                   "or else we'll fail to maintain line-info/flags "
                   "for EOL here");

        return identifierName(start, identStart, IdentifierEscapes::None,
                              modifier, NameVisibility::Public, ttp);
      }

      reportIllegalCharacter(cp);
      return badToken();
    }  // !isAsciiCodePoint(unit)

    consumeKnownCodeUnit(unit);

    // Get the token kind, based on the first char.  The ordering of c1kind
    // comparison is based on the frequency of tokens in real code:
    // Parsemark (which represents typical JS code on the web) and the
    // Unreal demo (which represents asm.js code).
    //
    //                  Parsemark   Unreal
    //  OneChar         32.9%       39.7%
    //  Space           25.0%        0.6%
    //  Ident           19.2%       36.4%
    //  Dec              7.2%        5.1%
    //  String           7.9%        0.0%
    //  EOL              1.7%        0.0%
    //  ZeroDigit        0.4%        4.9%
    //  Other            5.7%       13.3%
    //
    // The ordering is based mostly only Parsemark frequencies, with Unreal
    // frequencies used to break close categories (e.g. |Dec| and
    // |String|).  |Other| is biggish, but no other token kind is common
    // enough for it to be worth adding extra values to FirstCharKind.
    FirstCharKind c1kind = FirstCharKind(firstCharKinds[unit]);

    // Look for an unambiguous single-char token.
    //
    if (c1kind <= OneChar_Max) {
      TokenStart start(this->sourceUnits, -1);
      newSimpleToken(TokenKind(c1kind), start, modifier, ttp);
      return true;
    }

    // Skip over non-EOL whitespace chars.
    //
    if (c1kind == Space) {
      continue;
    }

    // Look for an identifier.
    //
    if (c1kind == Ident) {
      TokenStart start(this->sourceUnits, -1);
      return identifierName(
          start, this->sourceUnits.addressOfNextCodeUnit() - 1,
          IdentifierEscapes::None, modifier, NameVisibility::Public, ttp);
    }

    // Look for a decimal number.
    //
    if (c1kind == Dec) {
      TokenStart start(this->sourceUnits, -1);
      const Unit* numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;
      return decimalNumber(unit, start, numStart, modifier, ttp);
    }

    // Look for a string or a template string.
    //
    if (c1kind == String) {
      return getStringOrTemplateToken(static_cast<char>(unit), modifier, ttp);
    }

    // Skip over EOL chars, updating line state along the way.
    //
    if (c1kind == EOL) {
      if (unit == '\r') {
        matchLineTerminator('\n');
      }

      if (!updateLineInfoForEOL()) {
        return badToken();
      }

      anyCharsAccess().updateFlagsForEOL();
      continue;
    }

    // From a '0', look for a hexadecimal, binary, octal, or "noctal" (a
    // number starting with '0' that contains '8' or '9' and is treated as
    // decimal) number.
    //
    if (c1kind == ZeroDigit) {
      TokenStart start(this->sourceUnits, -1);
      int radix;
      bool isBigInt = false;
      const Unit* numStart;
      unit = getCodeUnit();
      if (unit == 'x' || unit == 'X') {
        radix = 16;
        unit = getCodeUnit();
        if (!IsAsciiHexDigit(unit)) {
          // NOTE: |unit| may be EOF here.
          ungetCodeUnit(unit);
          error(JSMSG_MISSING_HEXDIGITS);
          return badToken();
        }

        // one past the '0x'
        numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;

        if (!matchIntegerAfterFirstDigit(IsAsciiHexDigit, &unit)) {
          return badToken();
        }
      } else if (unit == 'b' || unit == 'B') {
        radix = 2;
        unit = getCodeUnit();
        if (!IsAsciiBinary(unit)) {
          // NOTE: |unit| may be EOF here.
          ungetCodeUnit(unit);
          error(JSMSG_MISSING_BINARY_DIGITS);
          return badToken();
        }

        // one past the '0b'
        numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;

        if (!matchIntegerAfterFirstDigit(IsAsciiBinary, &unit)) {
          return badToken();
        }
      } else if (unit == 'o' || unit == 'O') {
        radix = 8;
        unit = getCodeUnit();
        if (!IsAsciiOctal(unit)) {
          // NOTE: |unit| may be EOF here.
          ungetCodeUnit(unit);
          error(JSMSG_MISSING_OCTAL_DIGITS);
          return badToken();
        }

        // one past the '0o'
        numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;

        if (!matchIntegerAfterFirstDigit(IsAsciiOctal, &unit)) {
          return badToken();
        }
      } else if (IsAsciiDigit(unit)) {
        // Reject octal literals that appear in strict mode code.
        if (!strictModeError(JSMSG_DEPRECATED_OCTAL_LITERAL)) {
          return badToken();
        }

        // The above test doesn't catch a few edge cases; see
        // |GeneralParser::maybeParseDirective|.  Record the violation so that
        // that function can handle them.
        anyCharsAccess().setSawDeprecatedOctalLiteral();

        radix = 8;
        // one past the '0'
        numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;

        bool nonOctalDecimalIntegerLiteral = false;
        do {
          if (unit >= '8') {
            nonOctalDecimalIntegerLiteral = true;
          }
          unit = getCodeUnit();
        } while (IsAsciiDigit(unit));

        if (unit == '_') {
          ungetCodeUnit(unit);
          error(JSMSG_SEPARATOR_IN_ZERO_PREFIXED_NUMBER);
          return badToken();
        }

        if (unit == 'n') {
          ungetCodeUnit(unit);
          error(JSMSG_BIGINT_INVALID_SYNTAX);
          return badToken();
        }

        if (nonOctalDecimalIntegerLiteral) {
          // Use the decimal scanner for the rest of the number.
          return decimalNumber(unit, start, numStart, modifier, ttp);
        }
      } else if (unit == '_') {
        // Give a more explicit error message when '_' is used after '0'.
        ungetCodeUnit(unit);
        error(JSMSG_SEPARATOR_IN_ZERO_PREFIXED_NUMBER);
        return badToken();
      } else {
        // '0' not followed by [XxBbOo0-9_];  scan as a decimal number.
        ungetCodeUnit(unit);
        numStart = this->sourceUnits.addressOfNextCodeUnit() - 1;  // The '0'.
        return decimalNumber('0', start, numStart, modifier, ttp);
      }

      if (unit == 'n') {
        isBigInt = true;
        unit = peekCodeUnit();
      } else {
        ungetCodeUnit(unit);
      }

      // Error if an identifier-start code point appears immediately
      // after the number.  Somewhat surprisingly, if we don't check
      // here, we'll never check at all.
      if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
        if (unicode::IsIdentifierStart(char16_t(unit))) {
          error(JSMSG_IDSTART_AFTER_NUMBER);
          return badToken();
        }
      } else if (MOZ_LIKELY(unit != EOF)) {
        // This ignores encoding errors: subsequent caller-side code to
        // handle source text after the number will do so.
        PeekedCodePoint<Unit> peeked = this->sourceUnits.peekCodePoint();
        if (!peeked.isNone() &&
            unicode::IsIdentifierStart(peeked.codePoint())) {
          error(JSMSG_IDSTART_AFTER_NUMBER);
          return badToken();
        }
      }

      if (isBigInt) {
        return bigIntLiteral(start, modifier, ttp);
      }

      double dval;
      if (!GetFullInteger(numStart, this->sourceUnits.addressOfNextCodeUnit(),
                          radix, IntegerSeparatorHandling::SkipUnderscore,
                          &dval)) {
        ReportOutOfMemory(this->fc);
        return badToken();
      }
      newNumberToken(dval, NoDecimal, start, modifier, ttp);
      return true;
    }

    MOZ_ASSERT(c1kind == Other);

    // This handles everything else.  Simple tokens distinguished solely by
    // TokenKind should set |simpleKind| and break, to share simple-token
    // creation code for all such tokens.  All other tokens must be handled
    // by returning (or by continuing from the loop enclosing this).
    //
    TokenStart start(this->sourceUnits, -1);
    TokenKind simpleKind;
#ifdef DEBUG
    simpleKind = TokenKind::Limit;  // sentinel value for code after switch
#endif

    // The block a ways above eliminated all non-ASCII, so cast to the
    // smallest type possible to assist the C++ compiler.
    switch (AssertedCast<uint8_t>(CodeUnitValue(toUnit(unit)))) {
      case '.':
        if (IsAsciiDigit(peekCodeUnit())) {
          return decimalNumber('.', start,
                               this->sourceUnits.addressOfNextCodeUnit() - 1,
                               modifier, ttp);
        }

        unit = getCodeUnit();
        if (unit == '.') {
          if (matchCodeUnit('.')) {
            simpleKind = TokenKind::TripleDot;
            break;
          }
        }

        // NOTE: |unit| may be EOF here.  A stray '.' at EOF would be an
        //       error, but subsequent code will handle it.
        ungetCodeUnit(unit);

        simpleKind = TokenKind::Dot;
        break;

      case '#': {
#ifdef ENABLE_RECORD_TUPLE
        if (matchCodeUnit('{')) {
          simpleKind = TokenKind::HashCurly;
          break;
        }
        if (matchCodeUnit('[')) {
          simpleKind = TokenKind::HashBracket;
          break;
        }
#endif

        TokenStart start(this->sourceUnits, -1);
        const Unit* identStart = this->sourceUnits.addressOfNextCodeUnit() - 1;
        IdentifierEscapes sawEscape;
        if (!matchIdentifierStart(&sawEscape)) {
          return badToken();
        }
        return identifierName(start, identStart, sawEscape, modifier,
                              NameVisibility::Private, ttp);
      }

      case '=':
        if (matchCodeUnit('=')) {
          simpleKind = matchCodeUnit('=') ? TokenKind::StrictEq : TokenKind::Eq;
        } else if (matchCodeUnit('>')) {
          simpleKind = TokenKind::Arrow;
        } else {
          simpleKind = TokenKind::Assign;
        }
        break;

      case '+':
        if (matchCodeUnit('+')) {
          simpleKind = TokenKind::Inc;
        } else {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::AddAssign : TokenKind::Add;
        }
        break;

      case '\\': {
        char32_t codePoint;
        if (uint32_t escapeLength = matchUnicodeEscapeIdStart(&codePoint)) {
          return identifierName(
              start,
              this->sourceUnits.addressOfNextCodeUnit() - escapeLength - 1,
              IdentifierEscapes::SawUnicodeEscape, modifier,
              NameVisibility::Public, ttp);
        }

        // We could point "into" a mistyped escape, e.g. for "\u{41H}" we
        // could point at the 'H'.  But we don't do that now, so the code
        // unit after the '\' isn't necessarily bad, so just point at the
        // start of the actually-invalid escape.
        ungetCodeUnit('\\');
        error(JSMSG_BAD_ESCAPE);
        return badToken();
      }

      case '|':
        if (matchCodeUnit('|')) {
          simpleKind = matchCodeUnit('=') ? TokenKind::OrAssign : TokenKind::Or;
        } else {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::BitOrAssign : TokenKind::BitOr;
        }
        break;

      case '^':
        simpleKind =
            matchCodeUnit('=') ? TokenKind::BitXorAssign : TokenKind::BitXor;
        break;

      case '&':
        if (matchCodeUnit('&')) {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::AndAssign : TokenKind::And;
        } else {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::BitAndAssign : TokenKind::BitAnd;
        }
        break;

      case '?':
        if (matchCodeUnit('.')) {
          unit = getCodeUnit();
          if (IsAsciiDigit(unit)) {
            // if the code unit is followed by a number, for example it has the
            // following form `<...> ?.5 <..> then it should be treated as a
            // ternary rather than as an optional chain
            simpleKind = TokenKind::Hook;
            ungetCodeUnit(unit);
            ungetCodeUnit('.');
          } else {
            ungetCodeUnit(unit);
            simpleKind = TokenKind::OptionalChain;
          }
        } else if (matchCodeUnit('?')) {
          simpleKind = matchCodeUnit('=') ? TokenKind::CoalesceAssign
                                          : TokenKind::Coalesce;
        } else {
          simpleKind = TokenKind::Hook;
        }
        break;

      case '!':
        if (matchCodeUnit('=')) {
          simpleKind = matchCodeUnit('=') ? TokenKind::StrictNe : TokenKind::Ne;
        } else {
          simpleKind = TokenKind::Not;
        }
        break;

      case '<':
        if (anyCharsAccess().options().allowHTMLComments) {
          // Treat HTML begin-comment as comment-till-end-of-line.
          if (matchCodeUnit('!')) {
            if (matchCodeUnit('-')) {
              if (matchCodeUnit('-')) {
                this->sourceUnits.consumeRestOfSingleLineComment();
                continue;
              }
              ungetCodeUnit('-');
            }
            ungetCodeUnit('!');
          }
        }
        if (matchCodeUnit('<')) {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::LshAssign : TokenKind::Lsh;
        } else {
          simpleKind = matchCodeUnit('=') ? TokenKind::Le : TokenKind::Lt;
        }
        break;

      case '>':
        if (matchCodeUnit('>')) {
          if (matchCodeUnit('>')) {
            simpleKind =
                matchCodeUnit('=') ? TokenKind::UrshAssign : TokenKind::Ursh;
          } else {
            simpleKind =
                matchCodeUnit('=') ? TokenKind::RshAssign : TokenKind::Rsh;
          }
        } else {
          simpleKind = matchCodeUnit('=') ? TokenKind::Ge : TokenKind::Gt;
        }
        break;

      case '*':
        if (matchCodeUnit('*')) {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::PowAssign : TokenKind::Pow;
        } else {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::MulAssign : TokenKind::Mul;
        }
        break;

      case '/':
        // Look for a single-line comment.
        if (matchCodeUnit('/')) {
          unit = getCodeUnit();
          if (unit == '@' || unit == '#') {
            bool shouldWarn = unit == '@';
            if (!getDirectives(false, shouldWarn)) {
              return false;
            }
          } else {
            // NOTE: |unit| may be EOF here.
            ungetCodeUnit(unit);
          }

          this->sourceUnits.consumeRestOfSingleLineComment();
          continue;
        }

        // Look for a multi-line comment.
        if (matchCodeUnit('*')) {
          TokenStreamAnyChars& anyChars = anyCharsAccess();
          unsigned linenoBefore = anyChars.lineno;

          do {
            int32_t unit = getCodeUnit();
            if (unit == EOF) {
              error(JSMSG_UNTERMINATED_COMMENT);
              return badToken();
            }

            if (unit == '*' && matchCodeUnit('/')) {
              break;
            }

            if (unit == '@' || unit == '#') {
              bool shouldWarn = unit == '@';
              if (!getDirectives(true, shouldWarn)) {
                return badToken();
              }
            } else if (MOZ_LIKELY(isAsciiCodePoint(unit))) {
              if (!getFullAsciiCodePoint(unit)) {
                return badToken();
              }
            } else {
              char32_t codePoint;
              if (!getNonAsciiCodePoint(unit, &codePoint)) {
                return badToken();
              }
            }
          } while (true);

          if (linenoBefore != anyChars.lineno) {
            anyChars.updateFlagsForEOL();
          }

          continue;
        }

        // Look for a regexp.
        if (modifier == SlashIsRegExp) {
          return regexpLiteral(start, ttp);
        }

        simpleKind = matchCodeUnit('=') ? TokenKind::DivAssign : TokenKind::Div;
        break;

      case '%':
        simpleKind = matchCodeUnit('=') ? TokenKind::ModAssign : TokenKind::Mod;
        break;

      case '-':
        if (matchCodeUnit('-')) {
          if (anyCharsAccess().options().allowHTMLComments &&
              !anyCharsAccess().flags.isDirtyLine) {
            if (matchCodeUnit('>')) {
              this->sourceUnits.consumeRestOfSingleLineComment();
              continue;
            }
          }

          simpleKind = TokenKind::Dec;
        } else {
          simpleKind =
              matchCodeUnit('=') ? TokenKind::SubAssign : TokenKind::Sub;
        }
        break;

#ifdef ENABLE_DECORATORS
      case '@':
        simpleKind = TokenKind::At;
        break;
#endif

      default:
        // We consumed a bad ASCII code point/unit.  Put it back so the
        // error location is the bad code point.
        ungetCodeUnit(unit);
        reportIllegalCharacter(unit);
        return badToken();
    }  // switch (AssertedCast<uint8_t>(CodeUnitValue(toUnit(unit))))

    MOZ_ASSERT(simpleKind != TokenKind::Limit,
               "switch-statement should have set |simpleKind| before "
               "breaking");

    newSimpleToken(simpleKind, start, modifier, ttp);
    return true;
  } while (true);
}

template <typename Unit, class AnyCharsAccess>
bool TokenStreamSpecific<Unit, AnyCharsAccess>::getStringOrTemplateToken(
    char untilChar, Modifier modifier, TokenKind* out) {
  MOZ_ASSERT(untilChar == '\'' || untilChar == '"' || untilChar == '`',
             "unexpected string/template literal delimiter");

  bool parsingTemplate = (untilChar == '`');
  bool templateHead = false;

  TokenStart start(this->sourceUnits, -1);
  this->charBuffer.clear();

  // Run the bad-token code for every path out of this function except the
  // one success-case.
  auto noteBadToken = MakeScopeExit([this]() { this->badToken(); });

  auto ReportPrematureEndOfLiteral = [this, untilChar](unsigned errnum) {
    // Unicode separators aren't end-of-line in template or (as of
    // recently) string literals, so this assertion doesn't allow them.
    MOZ_ASSERT(this->sourceUnits.atEnd() ||
                   this->sourceUnits.peekCodeUnit() == Unit('\r') ||
                   this->sourceUnits.peekCodeUnit() == Unit('\n'),
               "must be parked at EOF or EOL to call this function");

    // The various errors reported here include language like "in a ''
    // literal" or similar, with '' being '', "", or `` as appropriate.
    const char delimiters[] = {untilChar, untilChar, '\0'};

    this->error(errnum, delimiters);
    return;
  };

  // We need to detect any of these chars:  " or ', \n (or its
  // equivalents), \\, EOF.  Because we detect EOL sequences here and
  // put them back immediately, we can use getCodeUnit().
  int32_t unit;
  while ((unit = getCodeUnit()) != untilChar) {
    if (unit == EOF) {
      ReportPrematureEndOfLiteral(JSMSG_EOF_BEFORE_END_OF_LITERAL);
      return false;
    }

    // Non-ASCII code points are always directly appended -- even
    // U+2028 LINE SEPARATOR and U+2029 PARAGRAPH SEPARATOR that are
    // ordinarily LineTerminatorSequences.  (They contribute their literal
    // values to template and [as of recently] string literals, but they're
    // line terminators when computing line/column coordinates.)  Handle
    // the non-ASCII case early for readability.
    if (MOZ_UNLIKELY(!isAsciiCodePoint(unit))) {
      char32_t cp;
      if (!getNonAsciiCodePointDontNormalize(toUnit(unit), &cp)) {
        return false;
      }

      if (MOZ_UNLIKELY(cp == unicode::LINE_SEPARATOR ||
                       cp == unicode::PARA_SEPARATOR)) {
        if (!updateLineInfoForEOL()) {
          return false;
        }

        anyCharsAccess().updateFlagsForEOL();
      } else {
        MOZ_ASSERT(!IsLineTerminator(cp));
      }

      if (!AppendCodePointToCharBuffer(this->charBuffer, cp)) {
        return false;
      }

      continue;
    }

    if (unit == '\\') {
      // When parsing templates, we don't immediately report errors for
      // invalid escapes; these are handled by the parser.  We don't
      // append to charBuffer in those cases because it won't be read.
      unit = getCodeUnit();
      if (unit == EOF) {
        ReportPrematureEndOfLiteral(JSMSG_EOF_IN_ESCAPE_IN_LITERAL);
        return false;
      }

      // Non-ASCII |unit| isn't handled by code after this, so dedicate
      // an unlikely special-case to it and then continue.
      if (MOZ_UNLIKELY(!isAsciiCodePoint(unit))) {
        char32_t codePoint;
        if (!getNonAsciiCodePoint(unit, &codePoint)) {
          return false;
        }

        // If we consumed U+2028 LINE SEPARATOR or U+2029 PARAGRAPH
        // SEPARATOR, they'll be normalized to '\n'.  '\' followed by
        // LineContinuation represents no code points, so don't append
        // in this case.
        if (codePoint != '\n') {
          if (!AppendCodePointToCharBuffer(this->charBuffer, codePoint)) {
            return false;
          }
        }

        continue;
      }

      // The block above eliminated all non-ASCII, so cast to the
      // smallest type possible to assist the C++ compiler.
      switch (AssertedCast<uint8_t>(CodeUnitValue(toUnit(unit)))) {
        case 'b':
          unit = '\b';
          break;
        case 'f':
          unit = '\f';
          break;
        case 'n':
          unit = '\n';
          break;
        case 'r':
          unit = '\r';
          break;
        case 't':
          unit = '\t';
          break;
        case 'v':
          unit = '\v';
          break;

        case '\r':
          matchLineTerminator('\n');
          [[fallthrough]];
        case '\n': {
          // LineContinuation represents no code points.  We're manually
          // consuming a LineTerminatorSequence, so we must manually
          // update line/column info.
          if (!updateLineInfoForEOL()) {
            return false;
          }

          continue;
        }

        // Unicode character specification.
        case 'u': {
          int32_t c2 = getCodeUnit();
          if (c2 == EOF) {
            ReportPrematureEndOfLiteral(JSMSG_EOF_IN_ESCAPE_IN_LITERAL);
            return false;
          }

          // First handle a delimited Unicode escape, e.g. \u{1F4A9}.
          if (c2 == '{') {
            uint32_t start = this->sourceUnits.offset() - 3;
            uint32_t code = 0;
            bool first = true;
            bool valid = true;
            do {
              int32_t u3 = getCodeUnit();
              if (u3 == EOF) {
                if (parsingTemplate) {
                  TokenStreamAnyChars& anyChars = anyCharsAccess();
                  anyChars.setInvalidTemplateEscape(start,
                                                    InvalidEscapeType::Unicode);
                  valid = false;
                  break;
                }
                reportInvalidEscapeError(start, InvalidEscapeType::Unicode);
                return false;
              }
              if (u3 == '}') {
                if (first) {
                  if (parsingTemplate) {
                    TokenStreamAnyChars& anyChars = anyCharsAccess();
                    anyChars.setInvalidTemplateEscape(
                        start, InvalidEscapeType::Unicode);
                    valid = false;
                    break;
                  }
                  reportInvalidEscapeError(start, InvalidEscapeType::Unicode);
                  return false;
                }
                break;
              }

              // Beware: |u3| may be a non-ASCII code point here; if
              // so it'll pass into this |if|-block.
              if (!IsAsciiHexDigit(u3)) {
                if (parsingTemplate) {
                  // We put the code unit back so that we read it
                  // on the next pass, which matters if it was
                  // '`' or '\'.
                  ungetCodeUnit(u3);

                  TokenStreamAnyChars& anyChars = anyCharsAccess();
                  anyChars.setInvalidTemplateEscape(start,
                                                    InvalidEscapeType::Unicode);
                  valid = false;
                  break;
                }
                reportInvalidEscapeError(start, InvalidEscapeType::Unicode);
                return false;
              }

              code = (code << 4) | AsciiAlphanumericToNumber(u3);
              if (code > unicode::NonBMPMax) {
                if (parsingTemplate) {
                  TokenStreamAnyChars& anyChars = anyCharsAccess();
                  anyChars.setInvalidTemplateEscape(
                      start + 3, InvalidEscapeType::UnicodeOverflow);
                  valid = false;
                  break;
                }
                reportInvalidEscapeError(start + 3,
                                         InvalidEscapeType::UnicodeOverflow);
                return false;
              }

              first = false;
            } while (true);

            if (!valid) {
              continue;
            }

            MOZ_ASSERT(code <= unicode::NonBMPMax);
            if (!AppendCodePointToCharBuffer(this->charBuffer, code)) {
              return false;
            }

            continue;
          }  // end of delimited Unicode escape handling

          // Otherwise it must be a fixed-length \uXXXX Unicode escape.
          // If it isn't, this is usually an error -- but if this is a
          // template literal, we must defer error reporting because
          // malformed escapes are okay in *tagged* template literals.
          char16_t v;
          if (IsAsciiHexDigit(c2) && this->sourceUnits.matchHexDigits(3, &v)) {
            unit = (AsciiAlphanumericToNumber(c2) << 12) | v;
          } else {
            // Beware: |c2| may not be an ASCII code point here!
            ungetCodeUnit(c2);
            uint32_t start = this->sourceUnits.offset() - 2;
            if (parsingTemplate) {
              TokenStreamAnyChars& anyChars = anyCharsAccess();
              anyChars.setInvalidTemplateEscape(start,
                                                InvalidEscapeType::Unicode);
              continue;
            }
            reportInvalidEscapeError(start, InvalidEscapeType::Unicode);
            return false;
          }
          break;
        }  // case 'u'

        // Hexadecimal character specification.
        case 'x': {
          char16_t v;
          if (this->sourceUnits.matchHexDigits(2, &v)) {
            unit = v;
          } else {
            uint32_t start = this->sourceUnits.offset() - 2;
            if (parsingTemplate) {
              TokenStreamAnyChars& anyChars = anyCharsAccess();
              anyChars.setInvalidTemplateEscape(start,
                                                InvalidEscapeType::Hexadecimal);
              continue;
            }
            reportInvalidEscapeError(start, InvalidEscapeType::Hexadecimal);
            return false;
          }
          break;
        }

        default: {
          if (!IsAsciiOctal(unit)) {
            // \8 or \9 in an untagged template literal is a syntax error,
            // reported in GeneralParser::noSubstitutionUntaggedTemplate.
            //
            // Tagged template literals, however, may contain \8 and \9.  The
            // "cooked" representation of such a part will be |undefined|, and
            // the "raw" representation will contain the literal characters.
            //
            //   function f(parts) {
            //     assertEq(parts[0], undefined);
            //     assertEq(parts.raw[0], "\\8");
            //     return "composed";
            //   }
            //   assertEq(f`\8`, "composed");
            if (unit == '8' || unit == '9') {
              TokenStreamAnyChars& anyChars = anyCharsAccess();
              if (parsingTemplate) {
                anyChars.setInvalidTemplateEscape(
                    this->sourceUnits.offset() - 2,
                    InvalidEscapeType::EightOrNine);
                continue;
              }

              // \8 and \9 are forbidden in string literals in strict mode code.
              if (!strictModeError(JSMSG_DEPRECATED_EIGHT_OR_NINE_ESCAPE)) {
                return false;
              }

              // The above test doesn't catch a few edge cases; see
              // |GeneralParser::maybeParseDirective|.  Record the violation so
              // that that function can handle them.
              anyChars.setSawDeprecatedEightOrNineEscape();
            }
            break;
          }

          // Octal character specification.
          int32_t val = AsciiOctalToNumber(unit);

          unit = peekCodeUnit();
          if (MOZ_UNLIKELY(unit == EOF)) {
            ReportPrematureEndOfLiteral(JSMSG_EOF_IN_ESCAPE_IN_LITERAL);
            return false;
          }

          // Strict mode code allows only \0 followed by a non-digit.
          if (val != 0 || IsAsciiDigit(unit)) {
            TokenStreamAnyChars& anyChars = anyCharsAccess();
            if (parsingTemplate) {
              anyChars.setInvalidTemplateEscape(this->sourceUnits.offset() - 2,
                                                InvalidEscapeType::Octal);
              continue;
            }

            if (!strictModeError(JSMSG_DEPRECATED_OCTAL_ESCAPE)) {
              return false;
            }

            // The above test doesn't catch a few edge cases; see
            // |GeneralParser::maybeParseDirective|.  Record the violation so
            // that that function can handle them.
            anyChars.setSawDeprecatedOctalEscape();
          }

          if (IsAsciiOctal(unit)) {
            val = 8 * val + AsciiOctalToNumber(unit);
            consumeKnownCodeUnit(unit);

            unit = peekCodeUnit();
            if (MOZ_UNLIKELY(unit == EOF)) {
              ReportPrematureEndOfLiteral(JSMSG_EOF_IN_ESCAPE_IN_LITERAL);
              return false;
            }

            if (IsAsciiOctal(unit)) {
              int32_t save = val;
              val = 8 * val + AsciiOctalToNumber(unit);
              if (val <= 0xFF) {
                consumeKnownCodeUnit(unit);
              } else {
                val = save;
              }
            }
          }

          unit = char16_t(val);
          break;
        }  // default
      }    // switch (AssertedCast<uint8_t>(CodeUnitValue(toUnit(unit))))

      if (!this->charBuffer.append(unit)) {
        return false;
      }

      continue;
    }  // (unit == '\\')

    if (unit == '\r' || unit == '\n') {
      if (!parsingTemplate) {
        // String literals don't allow ASCII line breaks.
        ungetCodeUnit(unit);
        ReportPrematureEndOfLiteral(JSMSG_EOL_BEFORE_END_OF_STRING);
        return false;
      }

      if (unit == '\r') {
        unit = '\n';
        matchLineTerminator('\n');
      }

      if (!updateLineInfoForEOL()) {
        return false;
      }

      anyCharsAccess().updateFlagsForEOL();
    } else if (parsingTemplate && unit == '$' && matchCodeUnit('{')) {
      templateHead = true;
      break;
    }

    if (!this->charBuffer.append(unit)) {
      return false;
    }
  }

  TaggedParserAtomIndex atom = drainCharBufferIntoAtom();
  if (!atom) {
    return false;
  }

  noteBadToken.release();

  MOZ_ASSERT_IF(!parsingTemplate, !templateHead);

  TokenKind kind = !parsingTemplate ? TokenKind::String
                   : templateHead   ? TokenKind::TemplateHead
                                    : TokenKind::NoSubsTemplate;
  newAtomToken(kind, atom, start, modifier, out);
  return true;
}

const char* TokenKindToDesc(TokenKind tt) {
  switch (tt) {
#define EMIT_CASE(name, desc) \
  case TokenKind::name:       \
    return desc;
    FOR_EACH_TOKEN_KIND(EMIT_CASE)
#undef EMIT_CASE
    case TokenKind::Limit:
      MOZ_ASSERT_UNREACHABLE("TokenKind::Limit should not be passed.");
      break;
  }

  return "<bad TokenKind>";
}

#ifdef DEBUG
const char* TokenKindToString(TokenKind tt) {
  switch (tt) {
#  define EMIT_CASE(name, desc) \
    case TokenKind::name:       \
      return "TokenKind::" #name;
    FOR_EACH_TOKEN_KIND(EMIT_CASE)
#  undef EMIT_CASE
    case TokenKind::Limit:
      break;
  }

  return "<bad TokenKind>";
}
#endif

template class TokenStreamCharsBase<Utf8Unit>;
template class TokenStreamCharsBase<char16_t>;

template class GeneralTokenStreamChars<char16_t, TokenStreamAnyCharsAccess>;
template class TokenStreamChars<char16_t, TokenStreamAnyCharsAccess>;
template class TokenStreamSpecific<char16_t, TokenStreamAnyCharsAccess>;

template class GeneralTokenStreamChars<
    Utf8Unit, ParserAnyCharsAccess<GeneralParser<FullParseHandler, Utf8Unit>>>;
template class GeneralTokenStreamChars<
    Utf8Unit,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, Utf8Unit>>>;
template class GeneralTokenStreamChars<
    char16_t, ParserAnyCharsAccess<GeneralParser<FullParseHandler, char16_t>>>;
template class GeneralTokenStreamChars<
    char16_t,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, char16_t>>>;

template class TokenStreamChars<
    Utf8Unit, ParserAnyCharsAccess<GeneralParser<FullParseHandler, Utf8Unit>>>;
template class TokenStreamChars<
    Utf8Unit,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, Utf8Unit>>>;
template class TokenStreamChars<
    char16_t, ParserAnyCharsAccess<GeneralParser<FullParseHandler, char16_t>>>;
template class TokenStreamChars<
    char16_t,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, char16_t>>>;

template class TokenStreamSpecific<
    Utf8Unit, ParserAnyCharsAccess<GeneralParser<FullParseHandler, Utf8Unit>>>;
template class TokenStreamSpecific<
    Utf8Unit,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, Utf8Unit>>>;
template class TokenStreamSpecific<
    char16_t, ParserAnyCharsAccess<GeneralParser<FullParseHandler, char16_t>>>;
template class TokenStreamSpecific<
    char16_t,
    ParserAnyCharsAccess<GeneralParser<SyntaxParseHandler, char16_t>>>;

}  // namespace frontend

}  // namespace js
