/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Streaming access to the raw tokens of JavaScript source.
 *
 * Because JS tokenization is context-sensitive -- a '/' could be either a
 * regular expression *or* a division operator depending on context -- the
 * various token stream classes are mostly not useful outside of the Parser
 * where they reside.  We should probably eventually merge the two concepts.
 */
#ifndef frontend_TokenStream_h
#define frontend_TokenStream_h

/*
 * [SMDOC] Parser Token Stream
 *
 * A token stream exposes the raw tokens -- operators, names, numbers,
 * keywords, and so on -- of JavaScript source code.
 *
 * These are the components of the overall token stream concept:
 * TokenStreamShared, TokenStreamAnyChars, TokenStreamCharsBase<Unit>,
 * TokenStreamChars<Unit>, and TokenStreamSpecific<Unit, AnyCharsAccess>.
 *
 * == TokenStreamShared → ∅ ==
 *
 * Certain aspects of tokenizing are used everywhere:
 *
 *   * modifiers (used to select which context-sensitive interpretation of a
 *     character should be used to decide what token it is) and modifier
 *     assertion handling;
 *   * flags on the overall stream (have we encountered any characters on this
 *     line?  have we hit a syntax error?  and so on);
 *   * and certain token-count constants.
 *
 * These are all defined in TokenStreamShared.  (They could be namespace-
 * scoped, but it seems tentatively better not to clutter the namespace.)
 *
 * == TokenStreamAnyChars → TokenStreamShared ==
 *
 * Certain aspects of tokenizing have meaning independent of the character type
 * of the source text being tokenized: line/column number information, tokens
 * in lookahead from determining the meaning of a prior token, compilation
 * options, the filename, flags, source map URL, access to details of the
 * current and next tokens (is the token of the given type?  what name or
 * number is contained in the token?  and other queries), and others.
 *
 * All this data/functionality *could* be duplicated for both single-byte and
 * double-byte tokenizing, but there are two problems.  First, it's potentially
 * wasteful if the compiler doesnt recognize it can unify the concepts.  (And
 * if any-character concepts are intermixed with character-specific concepts,
 * potentially the compiler *can't* unify them because offsets into the
 * hypothetical TokenStream<Unit>s would differ.)  Second, some of this stuff
 * needs to be accessible in ParserBase, the aspects of JS language parsing
 * that have meaning independent of the character type of the source text being
 * parsed.  So we need a separate data structure that ParserBase can hold on to
 * for it.  (ParserBase isn't the only instance of this, but it's certainly the
 * biggest case of it.)  Ergo, TokenStreamAnyChars.
 *
 * == TokenStreamCharsShared → ∅ ==
 *
 * Some functionality has meaning independent of character type, yet has no use
 * *unless* you know the character type in actual use.  It *could* live in
 * TokenStreamAnyChars, but it makes more sense to live in a separate class
 * that character-aware token information can simply inherit.
 *
 * This class currently exists only to contain a char16_t buffer, transiently
 * used to accumulate strings in tricky cases that can't just be read directly
 * from source text.  It's not used outside character-aware tokenizing, so it
 * doesn't make sense in TokenStreamAnyChars.
 *
 * == TokenStreamCharsBase<Unit> → TokenStreamCharsShared ==
 *
 * Certain data structures in tokenizing are character-type-specific: namely,
 * the various pointers identifying the source text (including current offset
 * and end).
 *
 * Additionally, some functions operating on this data are defined the same way
 * no matter what character type you have (e.g. current offset in code units
 * into the source text) or share a common interface regardless of character
 * type (e.g. consume the next code unit if it has a given value).
 *
 * All such functionality lives in TokenStreamCharsBase<Unit>.
 *
 * == SpecializedTokenStreamCharsBase<Unit> → TokenStreamCharsBase<Unit> ==
 *
 * Certain tokenizing functionality is specific to a single character type.
 * For example, JS's UTF-16 encoding recognizes no coding errors, because lone
 * surrogates are not an error; but a UTF-8 encoding must recognize a variety
 * of validation errors.  Such functionality is defined only in the appropriate
 * SpecializedTokenStreamCharsBase specialization.
 *
 * == GeneralTokenStreamChars<Unit, AnyCharsAccess> →
 *    SpecializedTokenStreamCharsBase<Unit> ==
 *
 * Some functionality operates differently on different character types, just
 * as for TokenStreamCharsBase, but additionally requires access to character-
 * type-agnostic information in TokenStreamAnyChars.  For example, getting the
 * next character performs different steps for different character types and
 * must access TokenStreamAnyChars to update line break information.
 *
 * Such functionality, if it can be defined using the same algorithm for all
 * character types, lives in GeneralTokenStreamChars<Unit, AnyCharsAccess>.
 * The AnyCharsAccess parameter provides a way for a GeneralTokenStreamChars
 * instance to access its corresponding TokenStreamAnyChars, without inheriting
 * from it.
 *
 * GeneralTokenStreamChars<Unit, AnyCharsAccess> is just functionality, no
 * actual member data.
 *
 * Such functionality all lives in TokenStreamChars<Unit, AnyCharsAccess>, a
 * declared-but-not-defined template class whose specializations have a common
 * public interface (plus whatever private helper functions are desirable).
 *
 * == TokenStreamChars<Unit, AnyCharsAccess> →
 *    GeneralTokenStreamChars<Unit, AnyCharsAccess> ==
 *
 * Some functionality is like that in GeneralTokenStreamChars, *but* it's
 * defined entirely differently for different character types.
 *
 * For example, consider "match a multi-code unit code point" (hypothetically:
 * we've only implemented two-byte tokenizing right now):
 *
 *   * For two-byte text, there must be two code units to get, the leading code
 *     unit must be a UTF-16 lead surrogate, and the trailing code unit must be
 *     a UTF-16 trailing surrogate.  (If any of these fail to hold, a next code
 *     unit encodes that code point and is not multi-code unit.)
 *   * For single-byte Latin-1 text, there are no multi-code unit code points.
 *   * For single-byte UTF-8 text, the first code unit must have N > 1 of its
 *     highest bits set (and the next unset), and |N - 1| successive code units
 *     must have their high bit set and next-highest bit unset, *and*
 *     concatenating all unconstrained bits together must not produce a code
 *     point value that could have been encoded in fewer code units.
 *
 * This functionality can't be implemented as member functions in
 * GeneralTokenStreamChars because we'd need to *partially specialize* those
 * functions -- hold Unit constant while letting AnyCharsAccess vary.  But
 * C++ forbids function template partial specialization like this: either you
 * fix *all* parameters or you fix none of them.
 *
 * Fortunately, C++ *does* allow *class* template partial specialization.  So
 * TokenStreamChars is a template class with one specialization per Unit.
 * Functions can be defined differently in the different specializations,
 * because AnyCharsAccess as the only template parameter on member functions
 * *can* vary.
 *
 * All TokenStreamChars<Unit, AnyCharsAccess> specializations, one per Unit,
 * are just functionality, no actual member data.
 *
 * == TokenStreamSpecific<Unit, AnyCharsAccess> →
 *    TokenStreamChars<Unit, AnyCharsAccess>, TokenStreamShared,
 *    ErrorReporter ==
 *
 * TokenStreamSpecific is operations that are parametrized on character type
 * but implement the *general* idea of tokenizing, without being intrinsically
 * tied to character type.  Notably, this includes all operations that can
 * report warnings or errors at particular offsets, because we include a line
 * of context with such errors -- and that necessarily accesses the raw
 * characters of their specific type.
 *
 * Much TokenStreamSpecific operation depends on functionality in
 * TokenStreamAnyChars.  The obvious solution is to inherit it -- but this
 * doesn't work in Parser: its ParserBase base class needs some
 * TokenStreamAnyChars functionality without knowing character type.
 *
 * The AnyCharsAccess type parameter is a class that statically converts from a
 * TokenStreamSpecific* to its corresponding TokenStreamAnyChars.  The
 * TokenStreamSpecific in Parser<ParseHandler, Unit> can then specify a class
 * that properly converts from TokenStreamSpecific Parser::tokenStream to
 * TokenStreamAnyChars ParserBase::anyChars.
 *
 * Could we hardcode one set of offset calculations for this and eliminate
 * AnyCharsAccess?  No.  Offset calculations possibly could be hardcoded if
 * TokenStreamSpecific were present in Parser before Parser::handler, assuring
 * the same offsets in all Parser-related cases.  But there's still a separate
 * TokenStream class, that requires different offset calculations.  So even if
 * we wanted to hardcode this (it's not clear we would, because forcing the
 * TokenStreamSpecific declarer to specify this is more explicit), we couldn't.
 */

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#include <algorithm>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <type_traits>

#include "jspubtd.h"

#include "frontend/ErrorReporter.h"
#include "frontend/ParserAtom.h"  // ParserAtom, ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/Token.h"
#include "frontend/TokenKind.h"
#include "js/CharacterEncoding.h"  // JS::ConstUTF8CharsZ
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::ColumnNumberOneOrigin, JS::ColumnNumberUnsignedOffset
#include "js/CompileOptions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/HashTable.h"             // js::HashMap
#include "js/RegExpFlags.h"           // JS::RegExpFlags
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "util/Unicode.h"
#include "vm/ErrorReporting.h"

struct KeywordInfo;

namespace js {

class FrontendContext;

namespace frontend {

// True if str is a keyword.
bool IsKeyword(TaggedParserAtomIndex atom);

// If `name` is reserved word, returns the TokenKind of it.
// TokenKind::Limit otherwise.
extern TokenKind ReservedWordTokenKind(TaggedParserAtomIndex name);

// If `name` is reserved word, returns string representation of it.
// nullptr otherwise.
extern const char* ReservedWordToCharZ(TaggedParserAtomIndex name);

// If `tt` is reserved word, returns string representation of it.
// nullptr otherwise.
extern const char* ReservedWordToCharZ(TokenKind tt);

enum class DeprecatedContent : uint8_t {
  // No deprecated content was present.
  None = 0,
  // Octal literal not prefixed by "0o" but rather by just "0", e.g. 0755.
  OctalLiteral,
  // Octal character escape, e.g. "hell\157 world".
  OctalEscape,
  // NonOctalDecimalEscape, i.e. "\8" or "\9".
  EightOrNineEscape,
};

struct TokenStreamFlags {
  // Hit end of file.
  bool isEOF : 1;
  // Non-whitespace since start of line.
  bool isDirtyLine : 1;
  // Hit a syntax error, at start or during a token.
  bool hadError : 1;

  // The nature of any deprecated content seen since last reset.
  // We have to uint8_t instead DeprecatedContent to work around a GCC 7 bug.
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61414
  uint8_t sawDeprecatedContent : 2;

  TokenStreamFlags()
      : isEOF(false),
        isDirtyLine(false),
        hadError(false),
        sawDeprecatedContent(uint8_t(DeprecatedContent::None)) {}
};

template <typename Unit>
class TokenStreamPosition;

/**
 * TokenStream types and constants that are used in both TokenStreamAnyChars
 * and TokenStreamSpecific.  Do not add any non-static data members to this
 * class!
 */
class TokenStreamShared {
 protected:
  // 1 current + (3 lookahead if EXPLICIT_RESOURCE_MANAGEMENT is enabled
  // else 2 lookahead and rounded up to ^2)
  // NOTE: This must be power of 2, in order to make `ntokensMask` work.
  static constexpr size_t ntokens = 4;

  static constexpr unsigned ntokensMask = ntokens - 1;

  template <typename Unit>
  friend class TokenStreamPosition;

 public:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
  // We need a lookahead buffer of atleast 3 for the AwaitUsing syntax.
  static constexpr unsigned maxLookahead = 3;
#else
  static constexpr unsigned maxLookahead = 2;
#endif

  using Modifier = Token::Modifier;
  static constexpr Modifier SlashIsDiv = Token::SlashIsDiv;
  static constexpr Modifier SlashIsRegExp = Token::SlashIsRegExp;
  static constexpr Modifier SlashIsInvalid = Token::SlashIsInvalid;

  static void verifyConsistentModifier(Modifier modifier,
                                       const Token& nextToken) {
    MOZ_ASSERT(
        modifier == nextToken.modifier || modifier == SlashIsInvalid,
        "This token was scanned with both SlashIsRegExp and SlashIsDiv, "
        "indicating the parser is confused about how to handle a slash here. "
        "See comment at Token::Modifier.");
  }
};

static_assert(std::is_empty_v<TokenStreamShared>,
              "TokenStreamShared shouldn't bloat classes that inherit from it");

template <typename Unit, class AnyCharsAccess>
class TokenStreamSpecific;

template <typename Unit>
class MOZ_STACK_CLASS TokenStreamPosition final {
 public:
  template <class AnyCharsAccess>
  inline explicit TokenStreamPosition(
      TokenStreamSpecific<Unit, AnyCharsAccess>& tokenStream);

 private:
  TokenStreamPosition(const TokenStreamPosition&) = delete;

  // Technically only TokenStreamSpecific<Unit, AnyCharsAccess>::seek with
  // Unit constant and AnyCharsAccess varying must be friended, but 1) it's
  // hard to friend one function in template classes, and 2) C++ doesn't
  // allow partial friend specialization to target just that single class.
  template <typename Char, class AnyCharsAccess>
  friend class TokenStreamSpecific;

  const Unit* buf;
  TokenStreamFlags flags;
  unsigned lineno;
  size_t linebase;
  size_t prevLinebase;
  Token currentToken;
  unsigned lookahead;
  Token lookaheadTokens[TokenStreamShared::maxLookahead];
};

template <typename Unit>
class SourceUnits;

/**
 * This class maps:
 *
 *   * a sourceUnits offset (a 0-indexed count of code units)
 *
 * to
 *
 *   * a (1-indexed) line number and
 *   * a (0-indexed) offset in code *units* (not code points, not bytes) into
 *     that line,
 *
 * for either |Unit = Utf8Unit| or |Unit = char16_t|.
 *
 * Note that, if |Unit = Utf8Unit|, the latter quantity is *not* the same as a
 * column number, which is a count of UTF-16 code units.  Computing a column
 * number requires the offset within the line and the source units of that line
 * (including what type |Unit| is, to know how to decode them).  If you need a
 * column number, functions in |GeneralTokenStreamChars<Unit>| will consult
 * this and source units to compute it.
 */
class SourceCoords {
  // For a given buffer holding source code, |lineStartOffsets_| has one
  // element per line of source code, plus one sentinel element.  Each
  // non-sentinel element holds the buffer offset for the start of the
  // corresponding line of source code.  For this example script,
  // assuming an initialLineOffset of 0:
  //
  // 1  // xyz            [line starts at offset 0]
  // 2  var x;            [line starts at offset 7]
  // 3                    [line starts at offset 14]
  // 4  var y;            [line starts at offset 15]
  //
  // |lineStartOffsets_| is:
  //
  //   [0, 7, 14, 15, MAX_PTR]
  //
  // To convert a "line number" to an "index" into |lineStartOffsets_|,
  // subtract |initialLineNum_|.  E.g. line 3's index is
  // (3 - initialLineNum_), which is 2.  Therefore lineStartOffsets_[2]
  // holds the buffer offset for the start of line 3, which is 14.  (Note
  // that |initialLineNum_| is often 1, but not always.
  //
  // The first element is always initialLineOffset, passed to the
  // constructor, and the last element is always the MAX_PTR sentinel.
  //
  // Offset-to-{line,offset-into-line} lookups are O(log n) in the worst
  // case (binary search), but in practice they're heavily clustered and
  // we do better than that by using the previous lookup's result
  // (lastIndex_) as a starting point.
  //
  // Checking if an offset lies within a particular line number
  // (isOnThisLine()) is O(1).
  //
  Vector<uint32_t, 128> lineStartOffsets_;

  /** The line number on which the source text begins. */
  uint32_t initialLineNum_;

  /**
   * The index corresponding to the last offset lookup -- used so that if
   * offset lookups proceed in increasing order, and and the offset appears
   * in the next couple lines from the last offset, we can avoid a full
   * binary-search.
   *
   * This is mutable because it's modified on every search, but that fact
   * isn't visible outside this class.
   */
  mutable uint32_t lastIndex_;

  uint32_t indexFromOffset(uint32_t offset) const;

  static const uint32_t MAX_PTR = UINT32_MAX;

  uint32_t lineNumberFromIndex(uint32_t index) const {
    return index + initialLineNum_;
  }

  uint32_t indexFromLineNumber(uint32_t lineNum) const {
    return lineNum - initialLineNum_;
  }

 public:
  SourceCoords(FrontendContext* fc, uint32_t initialLineNumber,
               uint32_t initialOffset);

  [[nodiscard]] bool add(uint32_t lineNum, uint32_t lineStartOffset);
  [[nodiscard]] bool fill(const SourceCoords& other);

  std::optional<bool> isOnThisLine(uint32_t offset, uint32_t lineNum) const {
    uint32_t index = indexFromLineNumber(lineNum);
    if (index + 1 >= lineStartOffsets_.length()) {  // +1 due to sentinel
      return std::nullopt;
    }
    return (lineStartOffsets_[index] <= offset &&
            offset < lineStartOffsets_[index + 1]);
  }

  /**
   * A token, computed for an offset in source text, that can be used to
   * access line number and line-offset information for that offset.
   *
   * LineToken *alone* exposes whether the corresponding offset is in the
   * the first line of source (which may not be 1, depending on
   * |initialLineNumber|), and whether it's in the same line as
   * another LineToken.
   */
  class LineToken {
    uint32_t index;
#ifdef DEBUG
    uint32_t offset_;  // stored for consistency-of-use assertions
#endif

    friend class SourceCoords;

   public:
    LineToken(uint32_t index, uint32_t offset)
        : index(index)
#ifdef DEBUG
          ,
          offset_(offset)
#endif
    {
    }

    bool isFirstLine() const { return index == 0; }

    bool isSameLine(LineToken other) const { return index == other.index; }

    void assertConsistentOffset(uint32_t offset) const {
      MOZ_ASSERT(offset_ == offset);
    }
  };

  /**
   * Compute a token usable to access information about the line at the
   * given offset.
   *
   * The only information directly accessible in a token is whether it
   * corresponds to the first line of source text (which may not be line
   * 1, depending on the |initialLineNumber| value used to construct
   * this).  Use |lineNumber(LineToken)| to compute the actual line
   * number (incorporating the contribution of |initialLineNumber|).
   */
  LineToken lineToken(uint32_t offset) const;

  /** Compute the line number for the given token. */
  uint32_t lineNumber(LineToken lineToken) const {
    return lineNumberFromIndex(lineToken.index);
  }

  /** Return the offset of the start of the line for |lineToken|. */
  uint32_t lineStart(LineToken lineToken) const {
    MOZ_ASSERT(lineToken.index + 1 < lineStartOffsets_.length(),
               "recorded line-start information must be available");
    return lineStartOffsets_[lineToken.index];
  }
};

enum class UnitsType : unsigned char {
  PossiblyMultiUnit = 0,
  GuaranteedSingleUnit = 1,
};

class ChunkInfo {
 private:
  // Column number offset in UTF-16 code units.
  // Store everything in |unsigned char|s so everything packs.
  unsigned char columnOffset_[sizeof(uint32_t)];
  unsigned char unitsType_;

 public:
  ChunkInfo(JS::ColumnNumberUnsignedOffset offset, UnitsType type)
      : unitsType_(static_cast<unsigned char>(type)) {
    memcpy(columnOffset_, offset.addressOfValueForTranscode(), sizeof(offset));
  }

  JS::ColumnNumberUnsignedOffset columnOffset() const {
    JS::ColumnNumberUnsignedOffset offset;
    memcpy(offset.addressOfValueForTranscode(), columnOffset_,
           sizeof(uint32_t));
    return offset;
  }

  UnitsType unitsType() const {
    MOZ_ASSERT(unitsType_ <= 1, "unitsType_ must be 0 or 1");
    return static_cast<UnitsType>(unitsType_);
  }

  void guaranteeSingleUnits() {
    MOZ_ASSERT(unitsType() == UnitsType::PossiblyMultiUnit,
               "should only be setting to possibly optimize from the "
               "pessimistic case");
    unitsType_ = static_cast<unsigned char>(UnitsType::GuaranteedSingleUnit);
  }
};

enum class InvalidEscapeType {
  // No invalid character escapes.
  None,
  // A malformed \x escape.
  Hexadecimal,
  // A malformed \u escape.
  Unicode,
  // An otherwise well-formed \u escape which represents a
  // codepoint > 10FFFF.
  UnicodeOverflow,
  // An octal escape in a template token.
  Octal,
  // NonOctalDecimalEscape - \8 or \9.
  EightOrNine
};

class TokenStreamAnyChars : public TokenStreamShared {
 private:
  // Constant-at-construction fields.

  FrontendContext* const fc;

  /** Options used for parsing/tokenizing. */
  const JS::ReadOnlyCompileOptions& options_;

  /**
   * Pointer used internally to test whether in strict mode.  Use |strictMode()|
   * instead of this field.
   */
  StrictModeGetter* const strictModeGetter_;

  /** Input filename or null. */
  JS::ConstUTF8CharsZ filename_;

  // Column number computation fields.
  // Used only for UTF-8 case.

  /**
   * A map of (line number => sequence of the column numbers at
   * |ColumnChunkLength|-unit boundaries rewound [if needed] to the nearest code
   * point boundary).  (|TokenStreamAnyChars::computeColumnOffset| is the sole
   * user of |ColumnChunkLength| and therefore contains its definition.)
   *
   * Entries appear in this map only when a column computation of sufficient
   * distance is performed on a line -- and only when the column is beyond the
   * first |ColumnChunkLength| units.  Each line's vector is lazily filled as
   * greater offsets require column computations.
   */
  mutable HashMap<uint32_t, Vector<ChunkInfo>> longLineColumnInfo_;

  // Computing accurate column numbers requires at *some* point linearly
  // iterating through prior source units in the line, to properly account for
  // multi-unit code points.  This is quadratic if counting happens repeatedly.
  //
  // But usually we need columns for advancing offsets through scripts.  By
  // caching the last ((line number, offset) => relative column) mapping (in
  // similar manner to how |SourceCoords::lastIndex_| is used to cache
  // (offset => line number) mappings) we can usually avoid re-iterating through
  // the common line prefix.
  //
  // Additionally, we avoid hash table lookup costs by caching the
  // |Vector<ChunkInfo>*| for the line of the last lookup.  (|nullptr| means we
  // must look it up -- or it hasn't been created yet.)  This pointer is nulled
  // when a lookup on a new line occurs, but as it's not a pointer at literal,
  // reallocatable element data, it's *not* invalidated when new entries are
  // added to such a vector.

  /**
   * The line in which the last column computation occurred, or UINT32_MAX if
   * no prior computation has yet happened.
   */
  mutable uint32_t lineOfLastColumnComputation_ = UINT32_MAX;

  /**
   * The chunk vector of the line for that last column computation.  This is
   * null if the chunk vector needs to be recalculated or initially created.
   */
  mutable Vector<ChunkInfo>* lastChunkVectorForLine_ = nullptr;

  /**
   * The offset (in code units) of the last column computation performed,
   * relative to source start.
   */
  mutable uint32_t lastOffsetOfComputedColumn_ = UINT32_MAX;

  /**
   * The column number offset from the 1st column for the offset (in code units)
   * of the last column computation performed, relative to source start.
   */
  mutable JS::ColumnNumberUnsignedOffset lastComputedColumnOffset_;

  // Intra-token fields.

  /**
   * The offset of the first invalid escape in a template literal.  (If there is
   * one -- if not, the value of this field is meaningless.)
   *
   * See also |invalidTemplateEscapeType|.
   */
  uint32_t invalidTemplateEscapeOffset = 0;

  /**
   * The type of the first invalid escape in a template literal.  (If there
   * isn't one, this will be |None|.)
   *
   * See also |invalidTemplateEscapeOffset|.
   */
  InvalidEscapeType invalidTemplateEscapeType = InvalidEscapeType::None;

  // Fields with values relevant across tokens (and therefore potentially across
  // function boundaries, such that lazy function parsing and stream-seeking
  // must take care in saving and restoring them).

  /** Line number and offset-to-line mapping information. */
  SourceCoords srcCoords;

  /** Circular token buffer of gotten tokens that have been ungotten. */
  Token tokens[ntokens] = {};

  /** The index in |tokens| of the last parsed token. */
  unsigned cursor_ = 0;

  /** The number of tokens in |tokens| available to be gotten. */
  unsigned lookahead = 0;

  /** The current line number. */
  unsigned lineno;

  /** Various flag bits (see above). */
  TokenStreamFlags flags = {};

  /** The offset of the start of the current line. */
  size_t linebase = 0;

  /** The start of the previous line, or |size_t(-1)| on the first line. */
  size_t prevLinebase = size_t(-1);

  /** The user's requested source URL.  Null if none has been set. */
  UniqueTwoByteChars displayURL_ = nullptr;

  /** The URL of the source map for this script.  Null if none has been set. */
  UniqueTwoByteChars sourceMapURL_ = nullptr;

  // Assorted boolean fields, none of which require maintenance across tokens,
  // stored at class end to minimize padding.

  /**
   * Whether syntax errors should or should not contain details about the
   * precise nature of the error.  (This is intended for use in suppressing
   * content-revealing details about syntax errors in cross-origin scripts on
   * the web.)
   */
  const bool mutedErrors;

  /**
   * An array storing whether a TokenKind observed while attempting to extend
   * a valid AssignmentExpression into an even longer AssignmentExpression
   * (e.g., extending '3' to '3 + 5') will terminate it without error.
   *
   * For example, ';' always ends an AssignmentExpression because it ends a
   * Statement or declaration.  '}' always ends an AssignmentExpression
   * because it terminates BlockStatement, FunctionBody, and embedded
   * expressions in TemplateLiterals.  Therefore both entries are set to true
   * in TokenStreamAnyChars construction.
   *
   * But e.g. '+' *could* extend an AssignmentExpression, so its entry here
   * is false.  Meanwhile 'this' can't extend an AssignmentExpression, but
   * it's only valid after a line break, so its entry here must be false.
   *
   * NOTE: This array could be static, but without C99's designated
   *       initializers it's easier zeroing here and setting the true entries
   *       in the constructor body.  (Having this per-instance might also aid
   *       locality.)  Don't worry!  Initialization time for each TokenStream
   *       is trivial.  See bug 639420.
   */
  bool isExprEnding[size_t(TokenKind::Limit)] = {};  // all-false initially

  // End of fields.

 public:
  TokenStreamAnyChars(FrontendContext* fc,
                      const JS::ReadOnlyCompileOptions& options,
                      StrictModeGetter* smg);

  template <typename Unit, class AnyCharsAccess>
  friend class GeneralTokenStreamChars;
  template <typename Unit, class AnyCharsAccess>
  friend class TokenStreamChars;
  template <typename Unit, class AnyCharsAccess>
  friend class TokenStreamSpecific;

  template <typename Unit>
  friend class TokenStreamPosition;

  // Accessors.
  unsigned cursor() const { return cursor_; }
  unsigned nextCursor() const { return (cursor_ + 1) & ntokensMask; }
  unsigned aheadCursor(unsigned steps) const {
    return (cursor_ + steps) & ntokensMask;
  }

  const Token& currentToken() const { return tokens[cursor()]; }
  bool isCurrentTokenType(TokenKind type) const {
    return currentToken().type == type;
  }

  [[nodiscard]] bool checkOptions();

 private:
  TaggedParserAtomIndex reservedWordToPropertyName(TokenKind tt) const;

 public:
  TaggedParserAtomIndex currentName() const {
    if (isCurrentTokenType(TokenKind::Name) ||
        isCurrentTokenType(TokenKind::PrivateName)) {
      return currentToken().name();
    }

    MOZ_ASSERT(TokenKindIsPossibleIdentifierName(currentToken().type));
    return reservedWordToPropertyName(currentToken().type);
  }

  bool currentNameHasEscapes(ParserAtomsTable& parserAtoms) const {
    if (isCurrentTokenType(TokenKind::Name) ||
        isCurrentTokenType(TokenKind::PrivateName)) {
      TokenPos pos = currentToken().pos;
      return (pos.end - pos.begin) != parserAtoms.length(currentToken().name());
    }

    MOZ_ASSERT(TokenKindIsPossibleIdentifierName(currentToken().type));
    return false;
  }

  bool isCurrentTokenAssignment() const {
    return TokenKindIsAssignment(currentToken().type);
  }

  // Flag methods.
  bool isEOF() const { return flags.isEOF; }
  bool hadError() const { return flags.hadError; }

  DeprecatedContent sawDeprecatedContent() const {
    return static_cast<DeprecatedContent>(flags.sawDeprecatedContent);
  }

 private:
  // Workaround GCC 7 sadness.
  void setSawDeprecatedContent(DeprecatedContent content) {
    flags.sawDeprecatedContent = static_cast<uint8_t>(content);
  }

 public:
  void clearSawDeprecatedContent() {
    setSawDeprecatedContent(DeprecatedContent::None);
  }
  void setSawDeprecatedOctalLiteral() {
    setSawDeprecatedContent(DeprecatedContent::OctalLiteral);
  }
  void setSawDeprecatedOctalEscape() {
    setSawDeprecatedContent(DeprecatedContent::OctalEscape);
  }
  void setSawDeprecatedEightOrNineEscape() {
    setSawDeprecatedContent(DeprecatedContent::EightOrNineEscape);
  }

  bool hasInvalidTemplateEscape() const {
    return invalidTemplateEscapeType != InvalidEscapeType::None;
  }
  void clearInvalidTemplateEscape() {
    invalidTemplateEscapeType = InvalidEscapeType::None;
  }

 private:
  // This is private because it should only be called by the tokenizer while
  // tokenizing not by, for example, BytecodeEmitter.
  bool strictMode() const {
    return strictModeGetter_ && strictModeGetter_->strictMode();
  }

  void setInvalidTemplateEscape(uint32_t offset, InvalidEscapeType type) {
    MOZ_ASSERT(type != InvalidEscapeType::None);
    if (invalidTemplateEscapeType != InvalidEscapeType::None) {
      return;
    }
    invalidTemplateEscapeOffset = offset;
    invalidTemplateEscapeType = type;
  }

 public:
  // Call this immediately after parsing an OrExpression to allow scanning the
  // next token with SlashIsRegExp without asserting (even though we just
  // peeked at it in SlashIsDiv mode).
  //
  // It's OK to disable the assertion because the places where this is called
  // have peeked at the next token in SlashIsDiv mode, and checked that it is
  // *not* a Div token.
  //
  // To see why it is necessary to disable the assertion, consider these two
  // programs:
  //
  //     x = arg => q       // per spec, this is all one statement, and the
  //     /a/g;              // slashes are division operators
  //
  //     x = arg => {}      // per spec, ASI at the end of this line
  //     /a/g;              // and that's a regexp literal
  //
  // The first program shows why orExpr() has use SlashIsDiv mode when peeking
  // ahead for the next operator after parsing `q`. The second program shows
  // why matchOrInsertSemicolon() must use SlashIsRegExp mode when scanning
  // ahead for a semicolon.
  void allowGettingNextTokenWithSlashIsRegExp() {
#ifdef DEBUG
    // Check the precondition: Caller already peeked ahead at the next token,
    // in SlashIsDiv mode, and it is *not* a Div token.
    MOZ_ASSERT(hasLookahead());
    const Token& next = nextToken();
    MOZ_ASSERT(next.modifier == SlashIsDiv);
    MOZ_ASSERT(next.type != TokenKind::Div);
    tokens[nextCursor()].modifier = SlashIsRegExp;
#endif
  }

#ifdef DEBUG
  inline bool debugHasNoLookahead() const { return lookahead == 0; }
#endif

  bool hasDisplayURL() const { return displayURL_ != nullptr; }

  char16_t* displayURL() { return displayURL_.get(); }

  bool hasSourceMapURL() const { return sourceMapURL_ != nullptr; }

  char16_t* sourceMapURL() { return sourceMapURL_.get(); }

  FrontendContext* context() const { return fc; }

  using LineToken = SourceCoords::LineToken;

  LineToken lineToken(uint32_t offset) const {
    return srcCoords.lineToken(offset);
  }

  uint32_t lineNumber(LineToken lineToken) const {
    return srcCoords.lineNumber(lineToken);
  }

  uint32_t lineStart(LineToken lineToken) const {
    return srcCoords.lineStart(lineToken);
  }

  /**
   * Fill in |err|.
   *
   * If the token stream doesn't have location info for this error, use the
   * caller's location (including line/column number) and return false.  (No
   * line of context is set.)
   *
   * Otherwise fill in everything in |err| except 1) line/column numbers and
   * 2) line-of-context-related fields and return true.  The caller *must*
   * fill in the line/column number; filling the line of context is optional.
   */
  bool fillExceptingContext(ErrorMetadata* err, uint32_t offset) const;

  MOZ_ALWAYS_INLINE void updateFlagsForEOL() { flags.isDirtyLine = false; }

 private:
  /**
   * Compute the column number offset from the 1st code unit in the line in
   * UTF-16 code units, for given absolute |offset| within source text on the
   * line of |lineToken| (which must have been computed from |offset|).
   *
   * A column number offset on a line that isn't the first line is just
   * the actual column number in 0-origin.  But a column number offset
   * on the first line is the column number offset from the initial
   * line/column of the script.  For example, consider this HTML with
   * line/column number keys:
   *
   *     Column number in 1-origin
   *                1         2            3
   *       123456789012345678901234   567890
   *
   *     Column number in 0-origin, and the offset from 1st column
   *                 1         2            3
   *       0123456789012345678901234   567890
   *     ------------------------------------
   *   1 | <html>
   *   2 | <head>
   *   3 |   <script>var x = 3;  x &lt; 4;
   *   4 | const y = 7;</script>
   *   5 | </head>
   *   6 | <body></body>
   *   7 | </html>
   *
   * The script would be compiled specifying initial (line, column) of (3, 10)
   * using |JS::ReadOnlyCompileOptions::{lineno,column}|, which is 0-origin.
   * And the column reported by |computeColumn| for the "v" of |var| would be
   * 11 (in 1-origin).  But the column number offset of the "v" in |var|, that
   * this function returns, would be 0.  On the other hand, the column reported
   * by |computeColumn| would be 1 (in 1-origin) and the column number offset
   * returned by this function for the "c" in |const| would be 0, because it's
   * not in the first line of source text.
   *
   * The column number offset is with respect *only* to the JavaScript source
   * text as SpiderMonkey sees it.  In the example, the "&lt;" is converted to
   * "<" by the browser before SpiderMonkey would see it.  So the column number
   * offset of the "4" in the inequality would be 16, not 19.
   *
   * UTF-16 code units are not all equal length in UTF-8 source, so counting
   * requires *some* kind of linear-time counting from the start of the line.
   * This function attempts various tricks to reduce this cost.  If these
   * optimizations succeed, repeated calls to this function on a line will pay
   * a one-time cost linear in the length of the line, then each call pays a
   * separate constant-time cost.  If the optimizations do not succeed, this
   * function works in time linear in the length of the line.
   *
   * It's unusual for a function in *this* class to be |Unit|-templated, but
   * while this operation manages |Unit|-agnostic fields in this class and in
   * |srcCoords|, it must *perform* |Unit|-sensitive computations to fill them.
   * And this is the best place to do that.
   */
  template <typename Unit>
  JS::ColumnNumberUnsignedOffset computeColumnOffset(
      const LineToken lineToken, const uint32_t offset,
      const SourceUnits<Unit>& sourceUnits) const;

  template <typename Unit>
  JS::ColumnNumberUnsignedOffset computeColumnOffsetForUTF8(
      const LineToken lineToken, const uint32_t offset, const uint32_t start,
      const uint32_t offsetInLine, const SourceUnits<Unit>& sourceUnits) const;

  /**
   * Update line/column information for the start of a new line at
   * |lineStartOffset|.
   */
  [[nodiscard]] MOZ_ALWAYS_INLINE bool internalUpdateLineInfoForEOL(
      uint32_t lineStartOffset);

 public:
  const Token& nextToken() const {
    MOZ_ASSERT(hasLookahead());
    return tokens[nextCursor()];
  }

  bool hasLookahead() const { return lookahead > 0; }

  void advanceCursor() { cursor_ = (cursor_ + 1) & ntokensMask; }

  void retractCursor() { cursor_ = (cursor_ - 1) & ntokensMask; }

  Token* allocateToken() {
    advanceCursor();

    Token* tp = &tokens[cursor()];
    MOZ_MAKE_MEM_UNDEFINED(tp, sizeof(*tp));

    return tp;
  }

  // Push the last scanned token back into the stream.
  void ungetToken() {
    MOZ_ASSERT(lookahead < maxLookahead);
    lookahead++;
    retractCursor();
  }

 public:
  void adoptState(TokenStreamAnyChars& other) {
    // If |other| has fresh information from directives, overwrite any
    // previously recorded directives.  (There is no specification directing
    // that last-in-source-order directive controls, sadly.  We behave this way
    // in the ordinary case, so we ought do so here too.)
    if (auto& url = other.displayURL_) {
      displayURL_ = std::move(url);
    }
    if (auto& url = other.sourceMapURL_) {
      sourceMapURL_ = std::move(url);
    }
  }

  // Compute error metadata for an error at no offset.
  void computeErrorMetadataNoOffset(ErrorMetadata* err) const;

  // ErrorReporter API Helpers

  // Provide minimal set of error reporting API given we cannot use
  // ErrorReportMixin here. "report" prefix is added to avoid conflict with
  // ErrorReportMixin methods in TokenStream class.
  void reportErrorNoOffset(unsigned errorNumber, ...) const;
  void reportErrorNoOffsetVA(unsigned errorNumber, va_list* args) const;

  const JS::ReadOnlyCompileOptions& options() const { return options_; }

  JS::ConstUTF8CharsZ getFilename() const { return filename_; }
};

constexpr char16_t CodeUnitValue(char16_t unit) { return unit; }

constexpr uint8_t CodeUnitValue(mozilla::Utf8Unit unit) {
  return unit.toUint8();
}

template <typename Unit>
class TokenStreamCharsBase;

template <typename T>
inline bool IsLineTerminator(T) = delete;

inline bool IsLineTerminator(char32_t codePoint) {
  return codePoint == '\n' || codePoint == '\r' ||
         codePoint == unicode::LINE_SEPARATOR ||
         codePoint == unicode::PARA_SEPARATOR;
}

inline bool IsLineTerminator(char16_t unit) {
  // Every LineTerminator fits in char16_t, so this is exact.
  return IsLineTerminator(static_cast<char32_t>(unit));
}

template <typename Unit>
struct SourceUnitTraits;

template <>
struct SourceUnitTraits<char16_t> {
 public:
  static constexpr uint8_t maxUnitsLength = 2;

  static constexpr size_t lengthInUnits(char32_t codePoint) {
    return codePoint < unicode::NonBMPMin ? 1 : 2;
  }
};

template <>
struct SourceUnitTraits<mozilla::Utf8Unit> {
 public:
  static constexpr uint8_t maxUnitsLength = 4;

  static constexpr size_t lengthInUnits(char32_t codePoint) {
    return codePoint < 0x80      ? 1
           : codePoint < 0x800   ? 2
           : codePoint < 0x10000 ? 3
                                 : 4;
  }
};

/**
 * PeekedCodePoint represents the result of peeking ahead in some source text
 * to determine the next validly-encoded code point.
 *
 * If there isn't a valid code point, then |isNone()|.
 *
 * But if there *is* a valid code point, then |!isNone()|, the code point has
 * value |codePoint()| and its length in code units is |lengthInUnits()|.
 *
 * Conceptually, this class is |Maybe<struct { char32_t v; uint8_t len; }>|.
 */
template <typename Unit>
class PeekedCodePoint final {
  char32_t codePoint_ = 0;
  uint8_t lengthInUnits_ = 0;

 private:
  using SourceUnitTraits = frontend::SourceUnitTraits<Unit>;

  PeekedCodePoint() = default;

 public:
  /**
   * Create a peeked code point with the given value and length in code
   * units.
   *
   * While the latter value is computable from the former for both UTF-8 and
   * JS's version of UTF-16, the caller likely computed a length in units in
   * the course of determining the peeked value.  Passing both here avoids
   * recomputation and lets us do a consistency-checking assertion.
   */
  PeekedCodePoint(char32_t codePoint, uint8_t lengthInUnits)
      : codePoint_(codePoint), lengthInUnits_(lengthInUnits) {
    MOZ_ASSERT(codePoint <= unicode::NonBMPMax);
    MOZ_ASSERT(lengthInUnits != 0, "bad code point length");
    MOZ_ASSERT(lengthInUnits == SourceUnitTraits::lengthInUnits(codePoint));
  }

  /** Create a PeekedCodeUnit that represents no valid code point. */
  static PeekedCodePoint none() { return PeekedCodePoint(); }

  /** True if no code point was found, false otherwise. */
  bool isNone() const { return lengthInUnits_ == 0; }

  /** If a code point was found, its value. */
  char32_t codePoint() const {
    MOZ_ASSERT(!isNone());
    return codePoint_;
  }

  /** If a code point was found, its length in code units. */
  uint8_t lengthInUnits() const {
    MOZ_ASSERT(!isNone());
    return lengthInUnits_;
  }
};

inline PeekedCodePoint<char16_t> PeekCodePoint(const char16_t* const ptr,
                                               const char16_t* const end) {
  if (MOZ_UNLIKELY(ptr >= end)) {
    return PeekedCodePoint<char16_t>::none();
  }

  char16_t lead = ptr[0];

  char32_t c;
  uint8_t len;
  if (MOZ_LIKELY(!unicode::IsLeadSurrogate(lead)) ||
      MOZ_UNLIKELY(ptr + 1 >= end || !unicode::IsTrailSurrogate(ptr[1]))) {
    c = lead;
    len = 1;
  } else {
    c = unicode::UTF16Decode(lead, ptr[1]);
    len = 2;
  }

  return PeekedCodePoint<char16_t>(c, len);
}

inline PeekedCodePoint<mozilla::Utf8Unit> PeekCodePoint(
    const mozilla::Utf8Unit* const ptr, const mozilla::Utf8Unit* const end) {
  if (MOZ_UNLIKELY(ptr >= end)) {
    return PeekedCodePoint<mozilla::Utf8Unit>::none();
  }

  const mozilla::Utf8Unit lead = ptr[0];
  if (mozilla::IsAscii(lead)) {
    return PeekedCodePoint<mozilla::Utf8Unit>(lead.toUint8(), 1);
  }

  const mozilla::Utf8Unit* afterLead = ptr + 1;
  mozilla::Maybe<char32_t> codePoint =
      mozilla::DecodeOneUtf8CodePoint(lead, &afterLead, end);
  if (codePoint.isNothing()) {
    return PeekedCodePoint<mozilla::Utf8Unit>::none();
  }

  auto len =
      mozilla::AssertedCast<uint8_t>(mozilla::PointerRangeSize(ptr, afterLead));
  MOZ_ASSERT(len <= 4);

  return PeekedCodePoint<mozilla::Utf8Unit>(codePoint.value(), len);
}

inline bool IsSingleUnitLineTerminator(mozilla::Utf8Unit unit) {
  // BEWARE: The Unicode line/paragraph separators don't fit in a single
  //         UTF-8 code unit, so this test is exact for Utf8Unit but inexact
  //         for UTF-8 as a whole.  Users must handle |unit| as start of a
  //         Unicode LineTerminator themselves!
  return unit == mozilla::Utf8Unit('\n') || unit == mozilla::Utf8Unit('\r');
}

// This is the low-level interface to the JS source code buffer.  It just gets
// raw Unicode code units -- 16-bit char16_t units of source text that are not
// (always) full code points, and 8-bit units of UTF-8 source text soon.
// TokenStreams functions are layered on top and do some extra stuff like
// converting all EOL sequences to '\n', tracking the line number, and setting
// |flags.isEOF|.  (The "raw" in "raw Unicode code units" refers to the lack of
// EOL sequence normalization.)
//
// buf[0..length-1] often represents a substring of some larger source,
// where we have only the substring in memory. The |startOffset| argument
// indicates the offset within this larger string at which our string
// begins, the offset of |buf[0]|.
template <typename Unit>
class SourceUnits {
 private:
  /** Base of buffer. */
  const Unit* base_;

  /** Offset of base_[0]. */
  uint32_t startOffset_;

  /** Limit for quick bounds check. */
  const Unit* limit_;

  /** Next char to get. */
  const Unit* ptr;

 public:
  SourceUnits(const Unit* units, size_t length, size_t startOffset)
      : base_(units),
        startOffset_(startOffset),
        limit_(units + length),
        ptr(units) {}

  bool atStart() const {
    MOZ_ASSERT(!isPoisoned(), "shouldn't be using if poisoned");
    return ptr == base_;
  }

  bool atEnd() const {
    MOZ_ASSERT(!isPoisoned(), "shouldn't be using if poisoned");
    MOZ_ASSERT(ptr <= limit_, "shouldn't have overrun");
    return ptr >= limit_;
  }

  size_t remaining() const {
    MOZ_ASSERT(!isPoisoned(),
               "can't get a count of remaining code units if poisoned");
    return mozilla::PointerRangeSize(ptr, limit_);
  }

  size_t startOffset() const { return startOffset_; }

  size_t offset() const {
    return startOffset_ + mozilla::PointerRangeSize(base_, ptr);
  }

  const Unit* codeUnitPtrAt(size_t offset) const {
    MOZ_ASSERT(!isPoisoned(), "shouldn't be using if poisoned");
    MOZ_ASSERT(startOffset_ <= offset);
    MOZ_ASSERT(offset - startOffset_ <=
               mozilla::PointerRangeSize(base_, limit_));
    return base_ + (offset - startOffset_);
  }

  const Unit* current() const { return ptr; }

  const Unit* limit() const { return limit_; }

  Unit previousCodeUnit() {
    MOZ_ASSERT(!isPoisoned(), "can't get previous code unit if poisoned");
    MOZ_ASSERT(!atStart(), "must have a previous code unit to get");
    return *(ptr - 1);
  }

  MOZ_ALWAYS_INLINE Unit getCodeUnit() {
    return *ptr++;  // this will nullptr-crash if poisoned
  }

  Unit peekCodeUnit() const {
    return *ptr;  // this will nullptr-crash if poisoned
  }

  /**
   * Determine the next code point in source text.  The code point is not
   * normalized: '\r', '\n', '\u2028', and '\u2029' are returned literally.
   * If there is no next code point because |atEnd()|, or if an encoding
   * error is encountered, return a |PeekedCodePoint| that |isNone()|.
   *
   * This function does not report errors: code that attempts to get the next
   * code point must report any error.
   *
   * If a next code point is found, it may be consumed by passing it to
   * |consumeKnownCodePoint|.
   */
  PeekedCodePoint<Unit> peekCodePoint() const {
    return PeekCodePoint(ptr, limit_);
  }

 private:
#ifdef DEBUG
  void assertNextCodePoint(const PeekedCodePoint<Unit>& peeked);
#endif

 public:
  /**
   * Consume a peeked code point that |!isNone()|.
   *
   * This call DOES NOT UPDATE LINE-STATUS.  You may need to call
   * |updateLineInfoForEOL()| and |updateFlagsForEOL()| if this consumes a
   * LineTerminator.  Note that if this consumes '\r', you also must consume
   * an optional '\n' (i.e. a full LineTerminatorSequence) before doing so.
   */
  void consumeKnownCodePoint(const PeekedCodePoint<Unit>& peeked) {
    MOZ_ASSERT(!peeked.isNone());
    MOZ_ASSERT(peeked.lengthInUnits() <= remaining());

#ifdef DEBUG
    assertNextCodePoint(peeked);
#endif

    ptr += peeked.lengthInUnits();
  }

  /** Match |n| hexadecimal digits and store their value in |*out|. */
  bool matchHexDigits(uint8_t n, char16_t* out) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't peek into poisoned SourceUnits");
    MOZ_ASSERT(n <= 4, "hexdigit value can't overflow char16_t");
    if (n > remaining()) {
      return false;
    }

    char16_t v = 0;
    for (uint8_t i = 0; i < n; i++) {
      auto unit = CodeUnitValue(ptr[i]);
      if (!mozilla::IsAsciiHexDigit(unit)) {
        return false;
      }

      v = (v << 4) | mozilla::AsciiAlphanumericToNumber(unit);
    }

    *out = v;
    ptr += n;
    return true;
  }

  bool matchCodeUnits(const char* chars, uint8_t length) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't match into poisoned SourceUnits");
    if (length > remaining()) {
      return false;
    }

    const Unit* start = ptr;
    const Unit* end = ptr + length;
    while (ptr < end) {
      if (*ptr++ != Unit(*chars++)) {
        ptr = start;
        return false;
      }
    }

    return true;
  }

  void skipCodeUnits(uint32_t n) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't use poisoned SourceUnits");
    MOZ_ASSERT(n <= remaining(), "shouldn't skip beyond end of SourceUnits");
    ptr += n;
  }

  void unskipCodeUnits(uint32_t n) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't use poisoned SourceUnits");
    MOZ_ASSERT(n <= mozilla::PointerRangeSize(base_, ptr),
               "shouldn't unskip beyond start of SourceUnits");
    ptr -= n;
  }

 private:
  friend class TokenStreamCharsBase<Unit>;

  bool internalMatchCodeUnit(Unit c) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't use poisoned SourceUnits");
    if (MOZ_LIKELY(!atEnd()) && *ptr == c) {
      ptr++;
      return true;
    }
    return false;
  }

 public:
  void consumeKnownCodeUnit(Unit c) {
    MOZ_ASSERT(!isPoisoned(), "shouldn't use poisoned SourceUnits");
    MOZ_ASSERT(*ptr == c, "consuming the wrong code unit");
    ptr++;
  }

  /** Unget U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR. */
  inline void ungetLineOrParagraphSeparator();

  void ungetCodeUnit() {
    MOZ_ASSERT(!isPoisoned(), "can't unget from poisoned units");
    MOZ_ASSERT(!atStart(), "can't unget if currently at start");
    ptr--;
  }

  const Unit* addressOfNextCodeUnit(bool allowPoisoned = false) const {
    MOZ_ASSERT_IF(!allowPoisoned, !isPoisoned());
    return ptr;
  }

  // Use this with caution!
  void setAddressOfNextCodeUnit(const Unit* a, bool allowPoisoned = false) {
    MOZ_ASSERT_IF(!allowPoisoned, a);
    ptr = a;
  }

  // Poison the SourceUnits so they can't be accessed again.
  void poisonInDebug() {
#ifdef DEBUG
    ptr = nullptr;
#endif
  }

 private:
  bool isPoisoned() const {
#ifdef DEBUG
    // |ptr| can be null for unpoisoned SourceUnits if this was initialized with
    // |units == nullptr| and |length == 0|.  In that case, for lack of any
    // better options, consider this to not be poisoned.
    return ptr == nullptr && ptr != limit_;
#else
    return false;
#endif
  }

 public:
  /**
   * Consume the rest of a single-line comment (but not the EOL/EOF that
   * terminates it).
   *
   * If an encoding error is encountered -- possible only for UTF-8 because
   * JavaScript's conception of UTF-16 encompasses any sequence of 16-bit
   * code units -- valid code points prior to the encoding error are consumed
   * and subsequent invalid code units are not consumed.  For example, given
   * these UTF-8 code units:
   *
   *   'B'   'A'  'D'  ':'   <bad code unit sequence>
   *   0x42  0x41 0x44 0x3A  0xD0 0x00 ...
   *
   * the first four code units are consumed, but 0xD0 and 0x00 are not
   * consumed because 0xD0 encodes a two-byte lead unit but 0x00 is not a
   * valid trailing code unit.
   *
   * It is expected that the caller will report such an encoding error when
   * it attempts to consume the next code point.
   */
  void consumeRestOfSingleLineComment();

  /**
   * The maximum radius of code around the location of an error that should
   * be included in a syntax error message -- this many code units to either
   * side.  The resulting window of data is then accordinngly trimmed so that
   * the window contains only validly-encoded data.
   *
   * Because this number is the same for both UTF-8 and UTF-16, windows in
   * UTF-8 may contain fewer code points than windows in UTF-16.  As we only
   * use this for error messages, we don't particularly care.
   */
  static constexpr size_t WindowRadius = ErrorMetadata::lineOfContextRadius;

  /**
   * From absolute offset |offset|, search backward to find an absolute
   * offset within source text, no further than |WindowRadius| code units
   * away from |offset|, such that all code points from that offset to
   * |offset| are valid, non-LineTerminator code points.
   */
  size_t findWindowStart(size_t offset) const;

  /**
   * From absolute offset |offset|, find an absolute offset within source
   * text, no further than |WindowRadius| code units away from |offset|, such
   * that all code units from |offset| to that offset are valid,
   * non-LineTerminator code points.
   */
  size_t findWindowEnd(size_t offset) const;

  /**
   * Given a |window| of |encodingSpecificWindowLength| units encoding valid
   * Unicode text, with index |encodingSpecificTokenOffset| indicating a
   * particular code point boundary in |window|, compute the corresponding
   * token offset and length if |window| were encoded in UTF-16.  For
   * example:
   *
   *   // U+03C0 GREEK SMALL LETTER PI is encoded as 0xCF 0x80.
   *   const Utf8Unit* encodedWindow =
   *     reinterpret_cast<const Utf8Unit*>(u8"ππππ = @ FAIL");
   *   size_t encodedTokenOffset = 11; // 2 * 4 + ' = '.length
   *   size_t encodedWindowLength = 17; // 2 * 4 + ' = @ FAIL'.length
   *   size_t utf16Offset, utf16Length;
   *   computeWindowOffsetAndLength(encodedWindow,
   *                                encodedTokenOffset, &utf16Offset,
   *                                encodedWindowLength, &utf16Length);
   *   MOZ_ASSERT(utf16Offset == 7);
   *   MOZ_ASSERT(utf16Length = 13);
   *
   * This function asserts if called for UTF-16: the sole caller can avoid
   * computing UTF-16 offsets when they're definitely the same as the encoded
   * offsets.
   */
  inline void computeWindowOffsetAndLength(const Unit* encodeWindow,
                                           size_t encodingSpecificTokenOffset,
                                           size_t* utf16TokenOffset,
                                           size_t encodingSpecificWindowLength,
                                           size_t* utf16WindowLength) const;
};

template <>
inline void SourceUnits<char16_t>::ungetLineOrParagraphSeparator() {
#ifdef DEBUG
  char16_t prev = previousCodeUnit();
#endif
  MOZ_ASSERT(prev == unicode::LINE_SEPARATOR ||
             prev == unicode::PARA_SEPARATOR);

  ungetCodeUnit();
}

template <>
inline void SourceUnits<mozilla::Utf8Unit>::ungetLineOrParagraphSeparator() {
  unskipCodeUnits(3);

  MOZ_ASSERT(ptr[0].toUint8() == 0xE2);
  MOZ_ASSERT(ptr[1].toUint8() == 0x80);

#ifdef DEBUG
  uint8_t last = ptr[2].toUint8();
#endif
  MOZ_ASSERT(last == 0xA8 || last == 0xA9);
}

/**
 * An all-purpose buffer type for accumulating text during tokenizing.
 *
 * In principle we could make this buffer contain |char16_t|, |Utf8Unit|, or
 * |Unit|.  We use |char16_t| because:
 *
 *   * we don't have a UTF-8 regular expression parser, so in general regular
 *     expression text must be copied to a separate UTF-16 buffer to parse it,
 *     and
 *   * |TokenStreamCharsShared::copyCharBufferTo|, which copies a shared
 *     |CharBuffer| to a |char16_t*|, is simpler if it doesn't have to convert.
 */
using CharBuffer = Vector<char16_t, 32>;

/**
 * Append the provided code point (in the range [U+0000, U+10FFFF], surrogate
 * code points included) to the buffer.
 */
[[nodiscard]] extern bool AppendCodePointToCharBuffer(CharBuffer& charBuffer,
                                                      char32_t codePoint);

/**
 * Accumulate the range of UTF-16 text (lone surrogates permitted, because JS
 * allows them in source text) into |charBuffer|.  Normalize '\r', '\n', and
 * "\r\n" into '\n'.
 */
[[nodiscard]] extern bool FillCharBufferFromSourceNormalizingAsciiLineBreaks(
    CharBuffer& charBuffer, const char16_t* cur, const char16_t* end);

/**
 * Accumulate the range of previously-validated UTF-8 text into |charBuffer|.
 * Normalize '\r', '\n', and "\r\n" into '\n'.
 */
[[nodiscard]] extern bool FillCharBufferFromSourceNormalizingAsciiLineBreaks(
    CharBuffer& charBuffer, const mozilla::Utf8Unit* cur,
    const mozilla::Utf8Unit* end);

class TokenStreamCharsShared {
 protected:
  FrontendContext* fc;

  /**
   * Buffer transiently used to store sequences of identifier or string code
   * points when such can't be directly processed from the original source
   * text (e.g. because it contains escapes).
   */
  CharBuffer charBuffer;

  /** Information for parsing with a lifetime longer than the parser itself. */
  ParserAtomsTable* parserAtoms;

 protected:
  explicit TokenStreamCharsShared(FrontendContext* fc,
                                  ParserAtomsTable* parserAtoms)
      : fc(fc), charBuffer(fc), parserAtoms(parserAtoms) {}

  [[nodiscard]] bool copyCharBufferTo(
      UniquePtr<char16_t[], JS::FreePolicy>* destination);

  /**
   * Determine whether a code unit constitutes a complete ASCII code point.
   * (The code point's exact value might not be used, however, if subsequent
   * code observes that |unit| is part of a LineTerminatorSequence.)
   */
  [[nodiscard]] static constexpr MOZ_ALWAYS_INLINE bool isAsciiCodePoint(
      int32_t unit) {
    return mozilla::IsAscii(static_cast<char32_t>(unit));
  }

  TaggedParserAtomIndex drainCharBufferIntoAtom() {
    // Add to parser atoms table.
    auto atom = this->parserAtoms->internChar16(fc, charBuffer.begin(),
                                                charBuffer.length());
    charBuffer.clear();
    return atom;
  }

 protected:
  void adoptState(TokenStreamCharsShared& other) {
    // The other stream's buffer may contain information for a
    // gotten-then-ungotten token, that we must transfer into this stream so
    // that token's final get behaves as desired.
    charBuffer = std::move(other.charBuffer);
  }

 public:
  CharBuffer& getCharBuffer() { return charBuffer; }
};

template <typename Unit>
class TokenStreamCharsBase : public TokenStreamCharsShared {
 protected:
  using SourceUnits = frontend::SourceUnits<Unit>;

  /** Code units in the source code being tokenized. */
  SourceUnits sourceUnits;

  // End of fields.

 protected:
  TokenStreamCharsBase(FrontendContext* fc, ParserAtomsTable* parserAtoms,
                       const Unit* units, size_t length, size_t startOffset);

  /**
   * Convert a non-EOF code unit returned by |getCodeUnit()| or
   * |peekCodeUnit()| to a Unit code unit.
   */
  inline Unit toUnit(int32_t codeUnitValue);

  void ungetCodeUnit(int32_t c) {
    if (c == EOF) {
      MOZ_ASSERT(sourceUnits.atEnd());
      return;
    }

    MOZ_ASSERT(sourceUnits.previousCodeUnit() == toUnit(c));
    sourceUnits.ungetCodeUnit();
  }

  MOZ_ALWAYS_INLINE TaggedParserAtomIndex
  atomizeSourceChars(mozilla::Span<const Unit> units);

  /**
   * Try to match a non-LineTerminator ASCII code point.  Return true iff it
   * was matched.
   */
  bool matchCodeUnit(char expect) {
    MOZ_ASSERT(mozilla::IsAscii(expect));
    MOZ_ASSERT(expect != '\r');
    MOZ_ASSERT(expect != '\n');
    return this->sourceUnits.internalMatchCodeUnit(Unit(expect));
  }

  /**
   * Try to match an ASCII LineTerminator code point.  Return true iff it was
   * matched.
   */
  MOZ_NEVER_INLINE bool matchLineTerminator(char expect) {
    MOZ_ASSERT(expect == '\r' || expect == '\n');
    return this->sourceUnits.internalMatchCodeUnit(Unit(expect));
  }

  template <typename T>
  bool matchCodeUnit(T) = delete;
  template <typename T>
  bool matchLineTerminator(T) = delete;

  int32_t peekCodeUnit() {
    return MOZ_LIKELY(!sourceUnits.atEnd())
               ? CodeUnitValue(sourceUnits.peekCodeUnit())
               : EOF;
  }

  /** Consume a known, non-EOF code unit. */
  inline void consumeKnownCodeUnit(int32_t unit);

  // Forbid accidental calls to consumeKnownCodeUnit *not* with the single
  // unit-or-EOF type.  Unit should use SourceUnits::consumeKnownCodeUnit;
  // CodeUnitValue() results should go through toUnit(), or better yet just
  // use the original Unit.
  template <typename T>
  inline void consumeKnownCodeUnit(T) = delete;

  /**
   * Add a null-terminated line of context to error information, for the line
   * in |sourceUnits| that contains |offset|.  Also record the window's
   * length and the offset of the error in the window.  (Don't bother adding
   * a line of context if it would be empty.)
   *
   * The window will contain no LineTerminators of any kind, and it will not
   * extend more than |SourceUnits::WindowRadius| to either side of |offset|,
   * nor into the previous or next lines.
   *
   * This function is quite internal, and you probably should be calling one
   * of its existing callers instead.
   */
  [[nodiscard]] bool addLineOfContext(ErrorMetadata* err,
                                      uint32_t offset) const;
};

template <>
inline char16_t TokenStreamCharsBase<char16_t>::toUnit(int32_t codeUnitValue) {
  MOZ_ASSERT(codeUnitValue != EOF, "EOF is not a Unit");
  return mozilla::AssertedCast<char16_t>(codeUnitValue);
}

template <>
inline mozilla::Utf8Unit TokenStreamCharsBase<mozilla::Utf8Unit>::toUnit(
    int32_t value) {
  MOZ_ASSERT(value != EOF, "EOF is not a Unit");
  return mozilla::Utf8Unit(mozilla::AssertedCast<unsigned char>(value));
}

template <typename Unit>
inline void TokenStreamCharsBase<Unit>::consumeKnownCodeUnit(int32_t unit) {
  sourceUnits.consumeKnownCodeUnit(toUnit(unit));
}

template <>
MOZ_ALWAYS_INLINE TaggedParserAtomIndex
TokenStreamCharsBase<char16_t>::atomizeSourceChars(
    mozilla::Span<const char16_t> units) {
  return this->parserAtoms->internChar16(fc, units.data(), units.size());
}

template <>
/* static */ MOZ_ALWAYS_INLINE TaggedParserAtomIndex
TokenStreamCharsBase<mozilla::Utf8Unit>::atomizeSourceChars(
    mozilla::Span<const mozilla::Utf8Unit> units) {
  return this->parserAtoms->internUtf8(fc, units.data(), units.size());
}

template <typename Unit>
class SpecializedTokenStreamCharsBase;

template <>
class SpecializedTokenStreamCharsBase<char16_t>
    : public TokenStreamCharsBase<char16_t> {
  using CharsBase = TokenStreamCharsBase<char16_t>;

 protected:
  using TokenStreamCharsShared::isAsciiCodePoint;
  // Deliberately don't |using| |sourceUnits| because of bug 1472569.  :-(

  using typename CharsBase::SourceUnits;

 protected:
  // These APIs are only usable by UTF-16-specific code.

  /**
   * Given |lead| already consumed, consume and return the code point encoded
   * starting from it.  Infallible because lone surrogates in JS encode a
   * "code point" of the same value.
   */
  char32_t infallibleGetNonAsciiCodePointDontNormalize(char16_t lead) {
    MOZ_ASSERT(!isAsciiCodePoint(lead));
    MOZ_ASSERT(this->sourceUnits.previousCodeUnit() == lead);

    // Handle single-unit code points and lone trailing surrogates.
    if (MOZ_LIKELY(!unicode::IsLeadSurrogate(lead)) ||
        // Or handle lead surrogates not paired with trailing surrogates.
        MOZ_UNLIKELY(
            this->sourceUnits.atEnd() ||
            !unicode::IsTrailSurrogate(this->sourceUnits.peekCodeUnit()))) {
      return lead;
    }

    // Otherwise it's a multi-unit code point.
    return unicode::UTF16Decode(lead, this->sourceUnits.getCodeUnit());
  }

 protected:
  // These APIs are in both SpecializedTokenStreamCharsBase specializations
  // and so are usable in subclasses no matter what Unit is.

  using CharsBase::CharsBase;
};

template <>
class SpecializedTokenStreamCharsBase<mozilla::Utf8Unit>
    : public TokenStreamCharsBase<mozilla::Utf8Unit> {
  using CharsBase = TokenStreamCharsBase<mozilla::Utf8Unit>;

 protected:
  // Deliberately don't |using| |sourceUnits| because of bug 1472569.  :-(

 protected:
  // These APIs are only usable by UTF-8-specific code.

  using typename CharsBase::SourceUnits;

  /**
   * A mutable iterator-wrapper around |SourceUnits| that translates
   * operators to calls to |SourceUnits::getCodeUnit()| and similar.
   *
   * This class is expected to be used in concert with |SourceUnitsEnd|.
   */
  class SourceUnitsIterator {
    SourceUnits& sourceUnits_;
#ifdef DEBUG
    // In iterator copies created by the post-increment operator, a pointer
    // at the next source text code unit when the post-increment operator
    // was called, cleared when the iterator is dereferenced.
    mutable mozilla::Maybe<const mozilla::Utf8Unit*>
        currentBeforePostIncrement_;
#endif

   public:
    explicit SourceUnitsIterator(SourceUnits& sourceUnits)
        : sourceUnits_(sourceUnits) {}

    mozilla::Utf8Unit operator*() const {
      // operator* is expected to get the *next* value from an iterator
      // not pointing at the end of the underlying range.  However, the
      // sole use of this is in the context of an expression of the form
      // |*iter++|, that performed the |sourceUnits_.getCodeUnit()| in
      // the |operator++(int)| below -- so dereferencing acts on a
      // |sourceUnits_| already advanced.  Therefore the correct unit to
      // return is the previous one.
      MOZ_ASSERT(currentBeforePostIncrement_.value() + 1 ==
                 sourceUnits_.current());
#ifdef DEBUG
      currentBeforePostIncrement_.reset();
#endif
      return sourceUnits_.previousCodeUnit();
    }

    SourceUnitsIterator operator++(int) {
      MOZ_ASSERT(currentBeforePostIncrement_.isNothing(),
                 "the only valid operation on a post-incremented "
                 "iterator is dereferencing a single time");

      SourceUnitsIterator copy = *this;
#ifdef DEBUG
      copy.currentBeforePostIncrement_.emplace(sourceUnits_.current());
#endif

      sourceUnits_.getCodeUnit();
      return copy;
    }

    void operator-=(size_t n) {
      MOZ_ASSERT(currentBeforePostIncrement_.isNothing(),
                 "the only valid operation on a post-incremented "
                 "iterator is dereferencing a single time");
      sourceUnits_.unskipCodeUnits(n);
    }

    mozilla::Utf8Unit operator[](ptrdiff_t index) {
      MOZ_ASSERT(currentBeforePostIncrement_.isNothing(),
                 "the only valid operation on a post-incremented "
                 "iterator is dereferencing a single time");
      MOZ_ASSERT(index == -1,
                 "must only be called to verify the value of the "
                 "previous code unit");
      return sourceUnits_.previousCodeUnit();
    }

    size_t remaining() const {
      MOZ_ASSERT(currentBeforePostIncrement_.isNothing(),
                 "the only valid operation on a post-incremented "
                 "iterator is dereferencing a single time");
      return sourceUnits_.remaining();
    }
  };

  /** A sentinel representing the end of |SourceUnits| data. */
  class SourceUnitsEnd {};

  friend inline size_t operator-(const SourceUnitsEnd& aEnd,
                                 const SourceUnitsIterator& aIter);

 protected:
  // These APIs are in both SpecializedTokenStreamCharsBase specializations
  // and so are usable in subclasses no matter what Unit is.

  using CharsBase::CharsBase;
};

inline size_t operator-(const SpecializedTokenStreamCharsBase<
                            mozilla::Utf8Unit>::SourceUnitsEnd& aEnd,
                        const SpecializedTokenStreamCharsBase<
                            mozilla::Utf8Unit>::SourceUnitsIterator& aIter) {
  return aIter.remaining();
}

/** A small class encapsulating computation of the start-offset of a Token. */
class TokenStart {
  uint32_t startOffset_;

 public:
  /**
   * Compute a starting offset that is the current offset of |sourceUnits|,
   * offset by |adjust|.  (For example, |adjust| of -1 indicates the code
   * unit one backwards from |sourceUnits|'s current offset.)
   */
  template <class SourceUnits>
  TokenStart(const SourceUnits& sourceUnits, ptrdiff_t adjust)
      : startOffset_(sourceUnits.offset() + adjust) {}

  TokenStart(const TokenStart&) = default;

  uint32_t offset() const { return startOffset_; }
};

template <typename Unit, class AnyCharsAccess>
class GeneralTokenStreamChars : public SpecializedTokenStreamCharsBase<Unit> {
  using CharsBase = TokenStreamCharsBase<Unit>;
  using SpecializedCharsBase = SpecializedTokenStreamCharsBase<Unit>;

  using LineToken = TokenStreamAnyChars::LineToken;

 private:
  Token* newTokenInternal(TokenKind kind, TokenStart start, TokenKind* out);

  /**
   * Allocates a new Token from the given offset to the current offset,
   * ascribes it the given kind, and sets |*out| to that kind.
   */
  Token* newToken(TokenKind kind, TokenStart start,
                  TokenStreamShared::Modifier modifier, TokenKind* out) {
    Token* token = newTokenInternal(kind, start, out);

#ifdef DEBUG
    // Save the modifier used to get this token, so that if an ungetToken()
    // occurs and then the token is re-gotten (or peeked, etc.), we can
    // assert both gets used compatible modifiers.
    token->modifier = modifier;
#endif

    return token;
  }

  uint32_t matchUnicodeEscape(char32_t* codePoint);
  uint32_t matchExtendedUnicodeEscape(char32_t* codePoint);

 protected:
  using CharsBase::addLineOfContext;
  using CharsBase::matchCodeUnit;
  using CharsBase::matchLineTerminator;
  using TokenStreamCharsShared::drainCharBufferIntoAtom;
  using TokenStreamCharsShared::isAsciiCodePoint;
  // Deliberately don't |using CharsBase::sourceUnits| because of bug 1472569.
  // :-(
  using CharsBase::toUnit;

  using typename CharsBase::SourceUnits;

 protected:
  using SpecializedCharsBase::SpecializedCharsBase;

  TokenStreamAnyChars& anyCharsAccess() {
    return AnyCharsAccess::anyChars(this);
  }

  const TokenStreamAnyChars& anyCharsAccess() const {
    return AnyCharsAccess::anyChars(this);
  }

  using TokenStreamSpecific =
      frontend::TokenStreamSpecific<Unit, AnyCharsAccess>;

  TokenStreamSpecific* asSpecific() {
    static_assert(
        std::is_base_of_v<GeneralTokenStreamChars, TokenStreamSpecific>,
        "static_cast below presumes an inheritance relationship");

    return static_cast<TokenStreamSpecific*>(this);
  }

 protected:
  /**
   * Compute the column number in Unicode code points of the absolute |offset|
   * within source text on the line corresponding to |lineToken|.
   *
   * |offset| must be a code point boundary, preceded only by validly-encoded
   * source units.  (It doesn't have to be *followed* by valid source units.)
   */
  JS::LimitedColumnNumberOneOrigin computeColumn(LineToken lineToken,
                                                 uint32_t offset) const;
  void computeLineAndColumn(uint32_t offset, uint32_t* line,
                            JS::LimitedColumnNumberOneOrigin* column) const;

  /**
   * Fill in |err| completely, except for line-of-context information.
   *
   * Return true if the caller can compute a line of context from the token
   * stream.  Otherwise return false.
   */
  [[nodiscard]] bool fillExceptingContext(ErrorMetadata* err,
                                          uint32_t offset) const {
    if (anyCharsAccess().fillExceptingContext(err, offset)) {
      JS::LimitedColumnNumberOneOrigin columnNumber;
      computeLineAndColumn(offset, &err->lineNumber, &columnNumber);
      err->columnNumber = JS::ColumnNumberOneOrigin(columnNumber);
      return true;
    }
    return false;
  }

  void newSimpleToken(TokenKind kind, TokenStart start,
                      TokenStreamShared::Modifier modifier, TokenKind* out) {
    newToken(kind, start, modifier, out);
  }

  void newNumberToken(double dval, DecimalPoint decimalPoint, TokenStart start,
                      TokenStreamShared::Modifier modifier, TokenKind* out) {
    Token* token = newToken(TokenKind::Number, start, modifier, out);
    token->setNumber(dval, decimalPoint);
  }

  void newBigIntToken(TokenStart start, TokenStreamShared::Modifier modifier,
                      TokenKind* out) {
    newToken(TokenKind::BigInt, start, modifier, out);
  }

  void newAtomToken(TokenKind kind, TaggedParserAtomIndex atom,
                    TokenStart start, TokenStreamShared::Modifier modifier,
                    TokenKind* out) {
    MOZ_ASSERT(kind == TokenKind::String || kind == TokenKind::TemplateHead ||
               kind == TokenKind::NoSubsTemplate);

    Token* token = newToken(kind, start, modifier, out);
    token->setAtom(atom);
  }

  void newNameToken(TaggedParserAtomIndex name, TokenStart start,
                    TokenStreamShared::Modifier modifier, TokenKind* out) {
    Token* token = newToken(TokenKind::Name, start, modifier, out);
    token->setName(name);
  }

  void newPrivateNameToken(TaggedParserAtomIndex name, TokenStart start,
                           TokenStreamShared::Modifier modifier,
                           TokenKind* out) {
    Token* token = newToken(TokenKind::PrivateName, start, modifier, out);
    token->setName(name);
  }

  void newRegExpToken(JS::RegExpFlags reflags, TokenStart start,
                      TokenKind* out) {
    Token* token = newToken(TokenKind::RegExp, start,
                            TokenStreamShared::SlashIsRegExp, out);
    token->setRegExpFlags(reflags);
  }

  MOZ_COLD bool badToken();

  /**
   * Get the next code unit -- the next numeric sub-unit of source text,
   * possibly smaller than a full code point -- without updating line/column
   * counters or consuming LineTerminatorSequences.
   *
   * Because of these limitations, only use this if (a) the resulting code
   * unit is guaranteed to be ungotten (by ungetCodeUnit()) if it's an EOL,
   * and (b) the line-related state (lineno, linebase) is not used before
   * it's ungotten.
   */
  int32_t getCodeUnit() {
    if (MOZ_LIKELY(!this->sourceUnits.atEnd())) {
      return CodeUnitValue(this->sourceUnits.getCodeUnit());
    }

    anyCharsAccess().flags.isEOF = true;
    return EOF;
  }

  void ungetCodeUnit(int32_t c) {
    MOZ_ASSERT_IF(c == EOF, anyCharsAccess().flags.isEOF);

    CharsBase::ungetCodeUnit(c);
  }

  /**
   * Given a just-consumed ASCII code unit/point |lead|, consume a full code
   * point or LineTerminatorSequence (normalizing it to '\n'). Return true on
   * success, otherwise return false.
   *
   * If a LineTerminatorSequence was consumed, also update line/column info.
   *
   * This may change the current |sourceUnits| offset.
   */
  [[nodiscard]] MOZ_ALWAYS_INLINE bool getFullAsciiCodePoint(int32_t lead) {
    MOZ_ASSERT(isAsciiCodePoint(lead),
               "non-ASCII code units must be handled separately");
    MOZ_ASSERT(toUnit(lead) == this->sourceUnits.previousCodeUnit(),
               "getFullAsciiCodePoint called incorrectly");

    if (MOZ_UNLIKELY(lead == '\r')) {
      matchLineTerminator('\n');
    } else if (MOZ_LIKELY(lead != '\n')) {
      return true;
    }
    return updateLineInfoForEOL();
  }

  [[nodiscard]] MOZ_NEVER_INLINE bool updateLineInfoForEOL() {
    return anyCharsAccess().internalUpdateLineInfoForEOL(
        this->sourceUnits.offset());
  }

  uint32_t matchUnicodeEscapeIdStart(char32_t* codePoint);
  bool matchUnicodeEscapeIdent(char32_t* codePoint);
  bool matchIdentifierStart();

  /**
   * If possible, compute a line of context for an otherwise-filled-in |err|
   * at the given offset in this token stream.
   *
   * This function is very-internal: almost certainly you should use one of
   * its callers instead.  It basically exists only to make those callers
   * more readable.
   */
  [[nodiscard]] bool internalComputeLineOfContext(ErrorMetadata* err,
                                                  uint32_t offset) const {
    // We only have line-start information for the current line.  If the error
    // is on a different line, we can't easily provide context.  (This means
    // any error in a multi-line token, e.g. an unterminated multiline string
    // literal, won't have context.)
    if (err->lineNumber != anyCharsAccess().lineno) {
      return true;
    }

    return addLineOfContext(err, offset);
  }

 public:
  /**
   * Consume any hashbang comment at the start of a Script or Module, if one is
   * present.  Stops consuming just before any terminating LineTerminator or
   * before an encoding error is encountered.
   */
  void consumeOptionalHashbangComment();

  TaggedParserAtomIndex getRawTemplateStringAtom() {
    TokenStreamAnyChars& anyChars = anyCharsAccess();

    MOZ_ASSERT(anyChars.currentToken().type == TokenKind::TemplateHead ||
               anyChars.currentToken().type == TokenKind::NoSubsTemplate);
    const Unit* cur =
        this->sourceUnits.codeUnitPtrAt(anyChars.currentToken().pos.begin + 1);
    const Unit* end;
    if (anyChars.currentToken().type == TokenKind::TemplateHead) {
      // Of the form    |`...${|   or   |}...${|
      end =
          this->sourceUnits.codeUnitPtrAt(anyChars.currentToken().pos.end - 2);
    } else {
      // NoSubsTemplate is of the form   |`...`|   or   |}...`|
      end =
          this->sourceUnits.codeUnitPtrAt(anyChars.currentToken().pos.end - 1);
    }

    // |charBuffer| should be empty here, but we may as well code defensively.
    MOZ_ASSERT(this->charBuffer.length() == 0);
    this->charBuffer.clear();

    // Template literals normalize only '\r' and "\r\n" to '\n'; Unicode
    // separators don't need special handling.
    // https://tc39.github.io/ecma262/#sec-static-semantics-tv-and-trv
    if (!FillCharBufferFromSourceNormalizingAsciiLineBreaks(this->charBuffer,
                                                            cur, end)) {
      return TaggedParserAtomIndex::null();
    }

    return drainCharBufferIntoAtom();
  }
};

template <typename Unit, class AnyCharsAccess>
class TokenStreamChars;

template <class AnyCharsAccess>
class TokenStreamChars<char16_t, AnyCharsAccess>
    : public GeneralTokenStreamChars<char16_t, AnyCharsAccess> {
  using CharsBase = TokenStreamCharsBase<char16_t>;
  using SpecializedCharsBase = SpecializedTokenStreamCharsBase<char16_t>;
  using GeneralCharsBase = GeneralTokenStreamChars<char16_t, AnyCharsAccess>;
  using Self = TokenStreamChars<char16_t, AnyCharsAccess>;

  using GeneralCharsBase::asSpecific;

  using typename GeneralCharsBase::TokenStreamSpecific;

 protected:
  using CharsBase::matchLineTerminator;
  using GeneralCharsBase::anyCharsAccess;
  using GeneralCharsBase::getCodeUnit;
  using SpecializedCharsBase::infallibleGetNonAsciiCodePointDontNormalize;
  using TokenStreamCharsShared::isAsciiCodePoint;
  // Deliberately don't |using| |sourceUnits| because of bug 1472569.  :-(
  using GeneralCharsBase::ungetCodeUnit;
  using GeneralCharsBase::updateLineInfoForEOL;

 protected:
  using GeneralCharsBase::GeneralCharsBase;

  /**
   * Given the non-ASCII |lead| code unit just consumed, consume and return a
   * complete non-ASCII code point.  Line/column updates are not performed,
   * and line breaks are returned as-is without normalization.
   */
  [[nodiscard]] bool getNonAsciiCodePointDontNormalize(char16_t lead,
                                                       char32_t* codePoint) {
    // There are no encoding errors in 16-bit JS, so implement this so that
    // the compiler knows it, too.
    *codePoint = infallibleGetNonAsciiCodePointDontNormalize(lead);
    return true;
  }

  /**
   * Given a just-consumed non-ASCII code unit |lead| (which may also be a
   * full code point, for UTF-16), consume a full code point or
   * LineTerminatorSequence (normalizing it to '\n') and store it in
   * |*codePoint|.  Return true on success, otherwise return false and leave
   * |*codePoint| undefined on failure.
   *
   * If a LineTerminatorSequence was consumed, also update line/column info.
   *
   * This may change the current |sourceUnits| offset.
   */
  [[nodiscard]] bool getNonAsciiCodePoint(int32_t lead, char32_t* codePoint);
};

template <class AnyCharsAccess>
class TokenStreamChars<mozilla::Utf8Unit, AnyCharsAccess>
    : public GeneralTokenStreamChars<mozilla::Utf8Unit, AnyCharsAccess> {
  using CharsBase = TokenStreamCharsBase<mozilla::Utf8Unit>;
  using SpecializedCharsBase =
      SpecializedTokenStreamCharsBase<mozilla::Utf8Unit>;
  using GeneralCharsBase =
      GeneralTokenStreamChars<mozilla::Utf8Unit, AnyCharsAccess>;
  using Self = TokenStreamChars<mozilla::Utf8Unit, AnyCharsAccess>;

  using typename SpecializedCharsBase::SourceUnitsEnd;
  using typename SpecializedCharsBase::SourceUnitsIterator;

 protected:
  using GeneralCharsBase::anyCharsAccess;
  using GeneralCharsBase::computeLineAndColumn;
  using GeneralCharsBase::fillExceptingContext;
  using GeneralCharsBase::internalComputeLineOfContext;
  using TokenStreamCharsShared::isAsciiCodePoint;
  // Deliberately don't |using| |sourceUnits| because of bug 1472569.  :-(
  using GeneralCharsBase::updateLineInfoForEOL;

 private:
  static char toHexChar(uint8_t nibble) {
    MOZ_ASSERT(nibble < 16);
    return "0123456789ABCDEF"[nibble];
  }

  static void byteToString(uint8_t n, char* str) {
    str[0] = '0';
    str[1] = 'x';
    str[2] = toHexChar(n >> 4);
    str[3] = toHexChar(n & 0xF);
  }

  static void byteToTerminatedString(uint8_t n, char* str) {
    byteToString(n, str);
    str[4] = '\0';
  }

  /**
   * Report a UTF-8 encoding-related error for a code point starting AT THE
   * CURRENT OFFSET.
   *
   * |relevantUnits| indicates how many code units from the current offset
   * are potentially relevant to the reported error, such that they may be
   * included in the error message.  For example, if at the current offset we
   * have
   *
   *   0b1111'1111 ...
   *
   * a code unit never allowed in UTF-8, then |relevantUnits| might be 1
   * because only that unit is relevant.  Or if we have
   *
   *   0b1111'0111 0b1011'0101 0b0000'0000 ...
   *
   * where the first two code units are a valid prefix to a four-unit code
   * point but the third unit *isn't* a valid trailing code unit, then
   * |relevantUnits| might be 3.
   */
  MOZ_COLD void internalEncodingError(uint8_t relevantUnits,
                                      unsigned errorNumber, ...);

  // Don't use |internalEncodingError|!  Use one of the elaborated functions
  // that calls it, below -- all of which should be used to indicate an error
  // in a code point starting AT THE CURRENT OFFSET as with
  // |internalEncodingError|.

  /** Report an error for an invalid lead code unit |lead|. */
  MOZ_COLD void badLeadUnit(mozilla::Utf8Unit lead);

  /**
   * Report an error when there aren't enough code units remaining to
   * constitute a full code point after |lead|: only |remaining| code units
   * were available for a code point starting with |lead|, when at least
   * |required| code units were required.
   */
  MOZ_COLD void notEnoughUnits(mozilla::Utf8Unit lead, uint8_t remaining,
                               uint8_t required);

  /**
   * Report an error for a bad trailing UTF-8 code unit, where the bad
   * trailing unit was the last of |unitsObserved| units examined from the
   * current offset.
   */
  MOZ_COLD void badTrailingUnit(uint8_t unitsObserved);

  // Helper used for both |badCodePoint| and |notShortestForm| for code units
  // that have all the requisite high bits set/unset in a manner that *could*
  // encode a valid code point, but the remaining bits encoding its actual
  // value do not define a permitted value.
  MOZ_COLD void badStructurallyValidCodePoint(char32_t codePoint,
                                              uint8_t codePointLength,
                                              const char* reason);

  /**
   * Report an error for UTF-8 that encodes a UTF-16 surrogate or a number
   * outside the Unicode range.
   */
  MOZ_COLD void badCodePoint(char32_t codePoint, uint8_t codePointLength) {
    MOZ_ASSERT(unicode::IsSurrogate(codePoint) ||
               codePoint > unicode::NonBMPMax);

    badStructurallyValidCodePoint(codePoint, codePointLength,
                                  unicode::IsSurrogate(codePoint)
                                      ? "it's a UTF-16 surrogate"
                                      : "the maximum code point is U+10FFFF");
  }

  /**
   * Report an error for UTF-8 that encodes a code point not in its shortest
   * form.
   */
  MOZ_COLD void notShortestForm(char32_t codePoint, uint8_t codePointLength) {
    MOZ_ASSERT(!unicode::IsSurrogate(codePoint));
    MOZ_ASSERT(codePoint <= unicode::NonBMPMax);

    badStructurallyValidCodePoint(
        codePoint, codePointLength,
        "it wasn't encoded in shortest possible form");
  }

 protected:
  using GeneralCharsBase::GeneralCharsBase;

  /**
   * Given the non-ASCII |lead| code unit just consumed, consume the rest of
   * a non-ASCII code point.  The code point is not normalized: on success
   * |*codePoint| may be U+2028 LINE SEPARATOR or U+2029 PARAGRAPH SEPARATOR.
   *
   * Report an error if an invalid code point is encountered.
   */
  [[nodiscard]] bool getNonAsciiCodePointDontNormalize(mozilla::Utf8Unit lead,
                                                       char32_t* codePoint);

  /**
   * Given a just-consumed non-ASCII code unit |lead|, consume a full code
   * point or LineTerminatorSequence (normalizing it to '\n') and store it in
   * |*codePoint|.  Return true on success, otherwise return false and leave
   * |*codePoint| undefined on failure.
   *
   * If a LineTerminatorSequence was consumed, also update line/column info.
   *
   * This function will change the current |sourceUnits| offset.
   */
  [[nodiscard]] bool getNonAsciiCodePoint(int32_t lead, char32_t* codePoint);
};

// TokenStream is the lexical scanner for JavaScript source text.
//
// It takes a buffer of Unit code units (currently only char16_t encoding
// UTF-16, but we're adding either UTF-8 or Latin-1 single-byte text soon) and
// linearly scans it into |Token|s.
//
// Internally the class uses a four element circular buffer |tokens| of
// |Token|s. As an index for |tokens|, the member |cursor_| points to the
// current token. Calls to getToken() increase |cursor_| by one and return the
// new current token. If a TokenStream was just created, the current token is
// uninitialized. It's therefore important that one of the first four member
// functions listed below is called first. The circular buffer lets us go back
// up to two tokens from the last scanned token. Internally, the relative
// number of backward steps that were taken (via ungetToken()) after the last
// token was scanned is stored in |lookahead|.
//
// The following table lists in which situations it is safe to call each listed
// function. No checks are made by the functions in non-debug builds.
//
// Function Name     | Precondition; changes to |lookahead|
// ------------------+---------------------------------------------------------
// getToken          | none; if |lookahead > 0| then |lookahead--|
// peekToken         | none; if |lookahead == 0| then |lookahead == 1|
// peekTokenSameLine | none; if |lookahead == 0| then |lookahead == 1|
// matchToken        | none; if |lookahead > 0| and the match succeeds then
//                   |       |lookahead--|
// consumeKnownToken | none; if |lookahead > 0| then |lookahead--|
// ungetToken        | 0 <= |lookahead| <= |maxLookahead - 1|; |lookahead++|
//
// The behavior of the token scanning process (see getTokenInternal()) can be
// modified by calling one of the first four above listed member functions with
// an optional argument of type Modifier.  However, the modifier will be
// ignored unless |lookahead == 0| holds.  Due to constraints of the grammar,
// this turns out not to be a problem in practice. See the
// mozilla.dev.tech.js-engine.internals thread entitled 'Bug in the scanner?'
// for more details:
// https://groups.google.com/forum/?fromgroups=#!topic/mozilla.dev.tech.js-engine.internals/2JLH5jRcr7E).
//
// The method seek() allows rescanning from a previously visited location of
// the buffer, initially computed by constructing a Position local variable.
//
template <typename Unit, class AnyCharsAccess>
class MOZ_STACK_CLASS TokenStreamSpecific
    : public TokenStreamChars<Unit, AnyCharsAccess>,
      public TokenStreamShared,
      public ErrorReporter {
 public:
  using CharsBase = TokenStreamCharsBase<Unit>;
  using SpecializedCharsBase = SpecializedTokenStreamCharsBase<Unit>;
  using GeneralCharsBase = GeneralTokenStreamChars<Unit, AnyCharsAccess>;
  using SpecializedChars = TokenStreamChars<Unit, AnyCharsAccess>;

  using Position = TokenStreamPosition<Unit>;

  // Anything inherited through a base class whose type depends upon this
  // class's template parameters can only be accessed through a dependent
  // name: prefixed with |this|, by explicit qualification, and so on.  (This
  // is so that references to inherited fields are statically distinguishable
  // from references to names outside of the class.)  This is tedious and
  // onerous.
  //
  // As an alternative, we directly add every one of these functions to this
  // class, using explicit qualification to address the dependent-name
  // problem.  |this| or other qualification is no longer necessary -- at
  // cost of this ever-changing laundry list of |using|s.  So it goes.
 public:
  using GeneralCharsBase::anyCharsAccess;
  using GeneralCharsBase::computeLineAndColumn;
  using TokenStreamCharsShared::adoptState;

 private:
  using typename CharsBase::SourceUnits;

 private:
  using CharsBase::atomizeSourceChars;
  using GeneralCharsBase::badToken;
  // Deliberately don't |using| |charBuffer| because of bug 1472569.  :-(
  using CharsBase::consumeKnownCodeUnit;
  using CharsBase::matchCodeUnit;
  using CharsBase::matchLineTerminator;
  using CharsBase::peekCodeUnit;
  using GeneralCharsBase::computeColumn;
  using GeneralCharsBase::fillExceptingContext;
  using GeneralCharsBase::getCodeUnit;
  using GeneralCharsBase::getFullAsciiCodePoint;
  using GeneralCharsBase::internalComputeLineOfContext;
  using GeneralCharsBase::matchUnicodeEscapeIdent;
  using GeneralCharsBase::matchUnicodeEscapeIdStart;
  using GeneralCharsBase::newAtomToken;
  using GeneralCharsBase::newBigIntToken;
  using GeneralCharsBase::newNameToken;
  using GeneralCharsBase::newNumberToken;
  using GeneralCharsBase::newPrivateNameToken;
  using GeneralCharsBase::newRegExpToken;
  using GeneralCharsBase::newSimpleToken;
  using SpecializedChars::getNonAsciiCodePoint;
  using SpecializedChars::getNonAsciiCodePointDontNormalize;
  using TokenStreamCharsShared::copyCharBufferTo;
  using TokenStreamCharsShared::drainCharBufferIntoAtom;
  using TokenStreamCharsShared::isAsciiCodePoint;
  // Deliberately don't |using| |sourceUnits| because of bug 1472569.  :-(
  using CharsBase::toUnit;
  using GeneralCharsBase::ungetCodeUnit;
  using GeneralCharsBase::updateLineInfoForEOL;

  template <typename CharU>
  friend class TokenStreamPosition;

 public:
  TokenStreamSpecific(FrontendContext* fc, ParserAtomsTable* parserAtoms,
                      const JS::ReadOnlyCompileOptions& options,
                      const Unit* units, size_t length);

  /**
   * Get the next code point, converting LineTerminatorSequences to '\n' and
   * updating internal line-counter state if needed. Return true on success.
   * Return false on failure.
   */
  [[nodiscard]] MOZ_ALWAYS_INLINE bool getCodePoint() {
    int32_t unit = getCodeUnit();
    if (MOZ_UNLIKELY(unit == EOF)) {
      MOZ_ASSERT(anyCharsAccess().flags.isEOF,
                 "flags.isEOF should have been set by getCodeUnit()");
      return true;
    }

    if (isAsciiCodePoint(unit)) {
      return getFullAsciiCodePoint(unit);
    }

    char32_t cp;
    return getNonAsciiCodePoint(unit, &cp);
  }

  // If there is an invalid escape in a template, report it and return false,
  // otherwise return true.
  bool checkForInvalidTemplateEscapeError() {
    if (anyCharsAccess().invalidTemplateEscapeType == InvalidEscapeType::None) {
      return true;
    }

    reportInvalidEscapeError(anyCharsAccess().invalidTemplateEscapeOffset,
                             anyCharsAccess().invalidTemplateEscapeType);
    return false;
  }

 public:
  // Implement ErrorReporter.

  std::optional<bool> isOnThisLine(size_t offset,
                                   uint32_t lineNum) const final {
    return anyCharsAccess().srcCoords.isOnThisLine(offset, lineNum);
  }

  uint32_t lineAt(size_t offset) const final {
    const auto& anyChars = anyCharsAccess();
    auto lineToken = anyChars.lineToken(offset);
    return anyChars.lineNumber(lineToken);
  }

  JS::LimitedColumnNumberOneOrigin columnAt(size_t offset) const final {
    return computeColumn(anyCharsAccess().lineToken(offset), offset);
  }

 private:
  // Implement ErrorReportMixin.

  FrontendContext* getContext() const override {
    return anyCharsAccess().context();
  }

  [[nodiscard]] bool strictMode() const override {
    return anyCharsAccess().strictMode();
  }

 public:
  // Implement ErrorReportMixin.

  const JS::ReadOnlyCompileOptions& options() const final {
    return anyCharsAccess().options();
  }

  [[nodiscard]] bool computeErrorMetadata(
      ErrorMetadata* err, const ErrorOffset& errorOffset) const override;

 private:
  void reportInvalidEscapeError(uint32_t offset, InvalidEscapeType type) {
    switch (type) {
      case InvalidEscapeType::None:
        MOZ_ASSERT_UNREACHABLE("unexpected InvalidEscapeType");
        return;
      case InvalidEscapeType::Hexadecimal:
        errorAt(offset, JSMSG_MALFORMED_ESCAPE, "hexadecimal");
        return;
      case InvalidEscapeType::Unicode:
        errorAt(offset, JSMSG_MALFORMED_ESCAPE, "Unicode");
        return;
      case InvalidEscapeType::UnicodeOverflow:
        errorAt(offset, JSMSG_UNICODE_OVERFLOW, "escape sequence");
        return;
      case InvalidEscapeType::Octal:
        errorAt(offset, JSMSG_DEPRECATED_OCTAL_ESCAPE);
        return;
      case InvalidEscapeType::EightOrNine:
        errorAt(offset, JSMSG_DEPRECATED_EIGHT_OR_NINE_ESCAPE);
        return;
    }
  }

  void reportIllegalCharacter(int32_t cp);

  [[nodiscard]] bool putIdentInCharBuffer(const Unit* identStart);

  using IsIntegerUnit = bool (*)(int32_t);
  [[nodiscard]] MOZ_ALWAYS_INLINE bool matchInteger(IsIntegerUnit isIntegerUnit,
                                                    int32_t* nextUnit);
  [[nodiscard]] MOZ_ALWAYS_INLINE bool matchIntegerAfterFirstDigit(
      IsIntegerUnit isIntegerUnit, int32_t* nextUnit);

  /**
   * Tokenize a decimal number that begins at |numStart| into the provided
   * token.
   *
   * |unit| must be one of these values:
   *
   *   1. The first decimal digit in the integral part of a decimal number
   *      not starting with '.', e.g. '1' for "17", '0' for "0.14", or
   *      '8' for "8.675309e6".
   *
   *   In this case, the next |getCodeUnit()| must return the code unit after
   *   |unit| in the overall number.
   *
   *   2. The '.' in a "."-prefixed decimal number, e.g. ".17" or ".1e3".
   *
   *   In this case, the next |getCodeUnit()| must return the code unit
   *   *after* the '.'.
   *
   *   3. (Non-strict mode code only)  The first non-ASCII-digit unit for a
   *      "noctal" number that begins with a '0' but contains a non-octal digit
   *      in its integer part so is interpreted as decimal, e.g. '.' in "09.28"
   *      or EOF for "0386" or '+' in "09+7" (three separate tokens).
   *
   *   In this case, the next |getCodeUnit()| returns the code unit after
   *   |unit|: '2', 'EOF', or '7' in the examples above.
   *
   * This interface is super-hairy and horribly stateful.  Unfortunately, its
   * hair merely reflects the intricacy of ECMAScript numeric literal syntax.
   * And incredibly, it *improves* on the goto-based horror that predated it.
   */
  [[nodiscard]] bool decimalNumber(int32_t unit, TokenStart start,
                                   const Unit* numStart, Modifier modifier,
                                   TokenKind* out);

  /** Tokenize a regular expression literal beginning at |start|. */
  [[nodiscard]] bool regexpLiteral(TokenStart start, TokenKind* out);

  /**
   * Slurp characters between |start| and sourceUnits.current() into
   * charBuffer, to later parse into a bigint.
   */
  [[nodiscard]] bool bigIntLiteral(TokenStart start, Modifier modifier,
                                   TokenKind* out);

 public:
  // Advance to the next token.  If the token stream encountered an error,
  // return false.  Otherwise return true and store the token kind in |*ttp|.
  [[nodiscard]] bool getToken(TokenKind* ttp, Modifier modifier = SlashIsDiv) {
    // Check for a pushed-back token resulting from mismatching lookahead.
    TokenStreamAnyChars& anyChars = anyCharsAccess();
    if (anyChars.lookahead != 0) {
      MOZ_ASSERT(!anyChars.flags.hadError);
      anyChars.lookahead--;
      anyChars.advanceCursor();
      TokenKind tt = anyChars.currentToken().type;
      MOZ_ASSERT(tt != TokenKind::Eol);
      verifyConsistentModifier(modifier, anyChars.currentToken());
      *ttp = tt;
      return true;
    }

    return getTokenInternal(ttp, modifier);
  }

  [[nodiscard]] bool peekToken(TokenKind* ttp, Modifier modifier = SlashIsDiv) {
    TokenStreamAnyChars& anyChars = anyCharsAccess();
    if (anyChars.lookahead > 0) {
      MOZ_ASSERT(!anyChars.flags.hadError);
      verifyConsistentModifier(modifier, anyChars.nextToken());
      *ttp = anyChars.nextToken().type;
      return true;
    }
    if (!getTokenInternal(ttp, modifier)) {
      return false;
    }
    anyChars.ungetToken();
    return true;
  }

  [[nodiscard]] bool peekTokenPos(TokenPos* posp,
                                  Modifier modifier = SlashIsDiv) {
    TokenStreamAnyChars& anyChars = anyCharsAccess();
    if (anyChars.lookahead == 0) {
      TokenKind tt;
      if (!getTokenInternal(&tt, modifier)) {
        return false;
      }
      anyChars.ungetToken();
      MOZ_ASSERT(anyChars.hasLookahead());
    } else {
      MOZ_ASSERT(!anyChars.flags.hadError);
      verifyConsistentModifier(modifier, anyChars.nextToken());
    }
    *posp = anyChars.nextToken().pos;
    return true;
  }

  [[nodiscard]] bool peekOffset(uint32_t* offset,
                                Modifier modifier = SlashIsDiv) {
    TokenPos pos;
    if (!peekTokenPos(&pos, modifier)) {
      return false;
    }
    *offset = pos.begin;
    return true;
  }

  // This is like peekToken(), with one exception:  if there is an EOL
  // between the end of the current token and the start of the next token, it
  // return true and store Eol in |*ttp|.  In that case, no token with
  // Eol is actually created, just a Eol TokenKind is returned, and
  // currentToken() shouldn't be consulted.  (This is the only place Eol
  // is produced.)
  [[nodiscard]] MOZ_ALWAYS_INLINE bool peekTokenSameLine(
      TokenKind* ttp, Modifier modifier = SlashIsDiv) {
    TokenStreamAnyChars& anyChars = anyCharsAccess();
    const Token& curr = anyChars.currentToken();

    // If lookahead != 0, we have scanned ahead at least one token, and
    // |lineno| is the line that the furthest-scanned token ends on.  If
    // it's the same as the line that the current token ends on, that's a
    // stronger condition than what we are looking for, and we don't need
    // to return Eol.
    if (anyChars.lookahead != 0) {
      std::optional<bool> onThisLineStatus =
          anyChars.srcCoords.isOnThisLine(curr.pos.end, anyChars.lineno);
      if (!onThisLineStatus.has_value()) {
        error(JSMSG_OUT_OF_MEMORY);
        return false;
      }

      bool onThisLine = *onThisLineStatus;
      if (onThisLine) {
        MOZ_ASSERT(!anyChars.flags.hadError);
        verifyConsistentModifier(modifier, anyChars.nextToken());
        *ttp = anyChars.nextToken().type;
        return true;
      }
    }

    // The above check misses two cases where we don't have to return
    // Eol.
    // - The next token starts on the same line, but is a multi-line token.
    // - The next token starts on the same line, but lookahead==2 and there
    //   is a newline between the next token and the one after that.
    // The following test is somewhat expensive but gets these cases (and
    // all others) right.
    TokenKind tmp;
    if (!getToken(&tmp, modifier)) {
      return false;
    }

    const Token& next = anyChars.currentToken();
    anyChars.ungetToken();

    // Careful, |next| points to an initialized-but-not-allocated Token!
    // This is safe because we don't modify token data below.

    auto currentEndToken = anyChars.lineToken(curr.pos.end);
    auto nextBeginToken = anyChars.lineToken(next.pos.begin);

    *ttp =
        currentEndToken.isSameLine(nextBeginToken) ? next.type : TokenKind::Eol;
    return true;
  }

  // Get the next token from the stream if its kind is |tt|.
  [[nodiscard]] bool matchToken(bool* matchedp, TokenKind tt,
                                Modifier modifier = SlashIsDiv) {
    TokenKind token;
    if (!getToken(&token, modifier)) {
      return false;
    }
    if (token == tt) {
      *matchedp = true;
    } else {
      anyCharsAccess().ungetToken();
      *matchedp = false;
    }
    return true;
  }

  void consumeKnownToken(TokenKind tt, Modifier modifier = SlashIsDiv) {
    bool matched;
    MOZ_ASSERT(anyCharsAccess().hasLookahead());
    MOZ_ALWAYS_TRUE(matchToken(&matched, tt, modifier));
    MOZ_ALWAYS_TRUE(matched);
  }

  [[nodiscard]] bool nextTokenEndsExpr(bool* endsExpr) {
    TokenKind tt;
    if (!peekToken(&tt)) {
      return false;
    }

    *endsExpr = anyCharsAccess().isExprEnding[size_t(tt)];
    if (*endsExpr) {
      // If the next token ends an overall Expression, we'll parse this
      // Expression without ever invoking Parser::orExpr().  But we need that
      // function's DEBUG-only side effect of marking this token as safe to get
      // with SlashIsRegExp, so we have to do it manually here.
      anyCharsAccess().allowGettingNextTokenWithSlashIsRegExp();
    }
    return true;
  }

  [[nodiscard]] bool advance(size_t position);

  void seekTo(const Position& pos);
  [[nodiscard]] bool seekTo(const Position& pos,
                            const TokenStreamAnyChars& other);

  void rewind(const Position& pos) {
    MOZ_ASSERT(pos.buf <= this->sourceUnits.addressOfNextCodeUnit(),
               "should be rewinding here");
    seekTo(pos);
  }

  [[nodiscard]] bool rewind(const Position& pos,
                            const TokenStreamAnyChars& other) {
    MOZ_ASSERT(pos.buf <= this->sourceUnits.addressOfNextCodeUnit(),
               "should be rewinding here");
    return seekTo(pos, other);
  }

  void fastForward(const Position& pos) {
    MOZ_ASSERT(this->sourceUnits.addressOfNextCodeUnit() <= pos.buf,
               "should be moving forward here");
    seekTo(pos);
  }

  [[nodiscard]] bool fastForward(const Position& pos,
                                 const TokenStreamAnyChars& other) {
    MOZ_ASSERT(this->sourceUnits.addressOfNextCodeUnit() <= pos.buf,
               "should be moving forward here");
    return seekTo(pos, other);
  }

  const Unit* codeUnitPtrAt(size_t offset) const {
    return this->sourceUnits.codeUnitPtrAt(offset);
  }

  [[nodiscard]] bool identifierName(TokenStart start, const Unit* identStart,
                                    IdentifierEscapes escaping,
                                    Modifier modifier,
                                    NameVisibility visibility, TokenKind* out);

  [[nodiscard]] bool matchIdentifierStart(IdentifierEscapes* sawEscape);

  [[nodiscard]] bool getTokenInternal(TokenKind* const ttp,
                                      const Modifier modifier);

  [[nodiscard]] bool getStringOrTemplateToken(char untilChar, Modifier modifier,
                                              TokenKind* out);

  // Parse a TemplateMiddle or TemplateTail token (one of the string-like parts
  // of a template string) after already consuming the leading `RightCurly`.
  // (The spec says the `}` is the first character of the TemplateMiddle/
  // TemplateTail, but we treat it as a separate token because that's much
  // easier to implement in both TokenStream and the parser.)
  //
  // This consumes a token and sets the current token, like `getToken()`.  It
  // doesn't take a Modifier because there's no risk of encountering a division
  // operator or RegExp literal.
  //
  // On success, `*ttp` is either `TokenKind::TemplateHead` (if we got a
  // TemplateMiddle token) or `TokenKind::NoSubsTemplate` (if we got a
  // TemplateTail). That may seem strange; there are four different template
  // token types in the spec, but we only use two. We use `TemplateHead` for
  // TemplateMiddle because both end with `...${`, and `NoSubsTemplate` for
  // TemplateTail because both contain the end of the template, including the
  // closing quote mark. They're not treated differently, either in the parser
  // or in the tokenizer.
  [[nodiscard]] bool getTemplateToken(TokenKind* ttp) {
    MOZ_ASSERT(anyCharsAccess().currentToken().type == TokenKind::RightCurly);
    return getStringOrTemplateToken('`', SlashIsInvalid, ttp);
  }

  [[nodiscard]] bool getDirectives(bool isMultiline, bool shouldWarnDeprecated);
  [[nodiscard]] bool getDirective(
      bool isMultiline, bool shouldWarnDeprecated, const char* directive,
      uint8_t directiveLength, const char* errorMsgPragma,
      UniquePtr<char16_t[], JS::FreePolicy>* destination);
  [[nodiscard]] bool getDisplayURL(bool isMultiline, bool shouldWarnDeprecated);
  [[nodiscard]] bool getSourceMappingURL(bool isMultiline,
                                         bool shouldWarnDeprecated);
};

// It's preferable to define this in TokenStream.cpp, but its template-ness
// means we'd then have to *instantiate* this constructor for all possible
// (Unit, AnyCharsAccess) pairs -- and that gets super-messy as AnyCharsAccess
// *itself* is templated.  This symbol really isn't that huge compared to some
// defined inline in TokenStreamSpecific, so just rely on the linker commoning
// stuff up.
template <typename Unit>
template <class AnyCharsAccess>
inline TokenStreamPosition<Unit>::TokenStreamPosition(
    TokenStreamSpecific<Unit, AnyCharsAccess>& tokenStream)
    : currentToken(tokenStream.anyCharsAccess().currentToken()) {
  TokenStreamAnyChars& anyChars = tokenStream.anyCharsAccess();

  buf =
      tokenStream.sourceUnits.addressOfNextCodeUnit(/* allowPoisoned = */ true);
  flags = anyChars.flags;
  lineno = anyChars.lineno;
  linebase = anyChars.linebase;
  prevLinebase = anyChars.prevLinebase;
  lookahead = anyChars.lookahead;
  currentToken = anyChars.currentToken();
  for (unsigned i = 0; i < anyChars.lookahead; i++) {
    lookaheadTokens[i] = anyChars.tokens[anyChars.aheadCursor(1 + i)];
  }
}

class TokenStreamAnyCharsAccess {
 public:
  template <class TokenStreamSpecific>
  static inline TokenStreamAnyChars& anyChars(TokenStreamSpecific* tss);

  template <class TokenStreamSpecific>
  static inline const TokenStreamAnyChars& anyChars(
      const TokenStreamSpecific* tss);
};

class MOZ_STACK_CLASS TokenStream
    : public TokenStreamAnyChars,
      public TokenStreamSpecific<char16_t, TokenStreamAnyCharsAccess> {
  using Unit = char16_t;

 public:
  TokenStream(FrontendContext* fc, ParserAtomsTable* parserAtoms,
              const JS::ReadOnlyCompileOptions& options, const Unit* units,
              size_t length, StrictModeGetter* smg)
      : TokenStreamAnyChars(fc, options, smg),
        TokenStreamSpecific<Unit, TokenStreamAnyCharsAccess>(
            fc, parserAtoms, options, units, length) {}
};

class MOZ_STACK_CLASS DummyTokenStream final : public TokenStream {
 public:
  DummyTokenStream(FrontendContext* fc,
                   const JS::ReadOnlyCompileOptions& options)
      : TokenStream(fc, nullptr, options, nullptr, 0, nullptr) {}
};

template <class TokenStreamSpecific>
/* static */ inline TokenStreamAnyChars& TokenStreamAnyCharsAccess::anyChars(
    TokenStreamSpecific* tss) {
  auto* ts = static_cast<TokenStream*>(tss);
  return *static_cast<TokenStreamAnyChars*>(ts);
}

template <class TokenStreamSpecific>
/* static */ inline const TokenStreamAnyChars&
TokenStreamAnyCharsAccess::anyChars(const TokenStreamSpecific* tss) {
  const auto* ts = static_cast<const TokenStream*>(tss);
  return *static_cast<const TokenStreamAnyChars*>(ts);
}

extern const char* TokenKindToDesc(TokenKind tt);

}  // namespace frontend
}  // namespace js

#ifdef DEBUG
extern const char* TokenKindToString(js::frontend::TokenKind tt);
#endif

#endif /* frontend_TokenStream_h */
