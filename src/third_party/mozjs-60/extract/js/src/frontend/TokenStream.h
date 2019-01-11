/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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
 * A token stream exposes the raw tokens -- operators, names, numbers,
 * keywords, and so on -- of JavaScript source code.
 *
 * These are the components of the overall token stream concept:
 * TokenStreamShared, TokenStreamAnyChars, TokenStreamCharsBase<CharT>,
 * TokenStreamChars<CharT>, and TokenStreamSpecific<CharT, AnyCharsAccess>.
 *
 * == TokenStreamShared → ∅ ==
 *
 * Certain aspects of tokenizing are used everywhere:
 *
 *   * modifiers (used to select which context-sensitive interpretation of a
 *     character should be used to decide what token it is), modifier
 *     exceptions, and modifier assertion handling;
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
 * hypothetical TokenStream<CharT>s would differ.)  Second, some of this stuff
 * needs to be accessible in ParserBase, the aspects of JS language parsing
 * that have meaning independent of the character type of the source text being
 * parsed.  So we need a separate data structure that ParserBase can hold on to
 * for it.  (ParserBase isn't the only instance of this, but it's certainly the
 * biggest case of it.)  Ergo, TokenStreamAnyChars.
 *
 * == TokenStreamCharsBase<CharT> → ∅ ==
 *
 * Certain data structures in tokenizing are character-type-specific:
 * the various pointers identifying the source text (including current offset
 * and end) , and the temporary vector into which characters are read/written
 * in certain cases (think writing out the actual codepoints identified by an
 * identifier containing a Unicode escape, to create the atom for the
 * identifier: |a\u0062c| versus |abc|, for example).
 *
 * Additionally, some functions operating on this data are defined the same way
 * no matter what character type you have -- the offset being |offset - start|
 * no matter whether those two variables are single- or double-byte pointers.
 *
 * All such functionality lives in TokenStreamCharsBase<CharT>.
 *
 * == GeneralTokenStreamChars<CharT, AnyCharsAccess> →
 *    TokenStreamCharsBase<CharT> ==
 *
 * Some functionality operates differently on different character types, just
 * as for TokenStreamCharsBase, but additionally requires access to character-
 * type-agnostic information in TokenStreamAnyChars.  For example, getting the
 * next character performs different steps for different character types and
 * must access TokenStreamAnyChars to update line break information.
 *
 * Such functionality, if it can be defined using the same algorithm for all
 * character types, lives in GeneralTokenStreamChars<CharT, AnyCharsAccess>.
 * The AnyCharsAccess parameter provides a way for a GeneralTokenStreamChars
 * instance to access its corresponding TokenStreamAnyChars, without inheriting
 * from it.
 *
 * GeneralTokenStreamChars<CharT, AnyCharsAccess> is just functionality, no
 * actual member data.
 *
 * Such functionality all lives in TokenStreamChars<CharT, AnyCharsAccess>, a
 * declared-but-not-defined template class whose specializations have a common
 * public interface (plus whatever private helper functions are desirable).
 *
 * == TokenStreamChars<CharT, AnyCharsAccess> →
 *    GeneralTokenStreamChars<CharT, AnyCharsAccess> ==
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
 * functions -- hold CharT constant while letting AnyCharsAccess vary.  But
 * C++ forbids function template partial specialization like this: either you
 * fix *all* parameters or you fix none of them.
 *
 * Fortunately, C++ *does* allow *class* template partial specialization.  So
 * TokenStreamChars is a template class with one specialization per CharT.
 * Functions can be defined differently in the different specializations,
 * because AnyCharsAccess as the only template parameter on member functions
 * *can* vary.
 *
 * All TokenStreamChars<CharT, AnyCharsAccess> specializations, one per CharT,
 * are just functionality, no actual member data.
 *
 * == TokenStreamSpecific<CharT, AnyCharsAccess> →
 *    TokenStreamChars<CharT, AnyCharsAccess>, TokenStreamShared ==
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
 * TokenStreamSpecific in Parser<ParseHandler, CharT> can then specify a class
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
#include "mozilla/DebugOnly.h"
#include "mozilla/PodOperations.h"
#include "mozilla/TypeTraits.h"
#include "mozilla/Unused.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "jspubtd.h"

#include "frontend/ErrorReporter.h"
#include "frontend/TokenKind.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "util/Unicode.h"
#include "vm/ErrorReporting.h"
#include "vm/JSContext.h"
#include "vm/RegExpShared.h"
#include "vm/StringType.h"

struct KeywordInfo;

namespace js {
namespace frontend {

struct TokenPos {
    uint32_t    begin;  // Offset of the token's first char.
    uint32_t    end;    // Offset of 1 past the token's last char.

    TokenPos() {}
    TokenPos(uint32_t begin, uint32_t end) : begin(begin), end(end) {}

    // Return a TokenPos that covers left, right, and anything in between.
    static TokenPos box(const TokenPos& left, const TokenPos& right) {
        MOZ_ASSERT(left.begin <= left.end);
        MOZ_ASSERT(left.end <= right.begin);
        MOZ_ASSERT(right.begin <= right.end);
        return TokenPos(left.begin, right.end);
    }

    bool operator==(const TokenPos& bpos) const {
        return begin == bpos.begin && end == bpos.end;
    }

    bool operator!=(const TokenPos& bpos) const {
        return begin != bpos.begin || end != bpos.end;
    }

    bool operator <(const TokenPos& bpos) const {
        return begin < bpos.begin;
    }

    bool operator <=(const TokenPos& bpos) const {
        return begin <= bpos.begin;
    }

    bool operator >(const TokenPos& bpos) const {
        return !(*this <= bpos);
    }

    bool operator >=(const TokenPos& bpos) const {
        return !(*this < bpos);
    }

    bool encloses(const TokenPos& pos) const {
        return begin <= pos.begin && pos.end <= end;
    }
};

enum DecimalPoint { NoDecimal = false, HasDecimal = true };

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
    Octal
};

class TokenStreamShared;

struct Token
{
  private:
    // Sometimes the parser needs to inform the tokenizer to interpret
    // subsequent text in a particular manner: for example, to tokenize a
    // keyword as an identifier, not as the actual keyword, on the right-hand
    // side of a dotted property access.  Such information is communicated to
    // the tokenizer as a Modifier when getting the next token.
    //
    // Ideally this definition would reside in TokenStream as that's the real
    // user, but the debugging-use of it here causes a cyclic dependency (and
    // C++ provides no way to forward-declare an enum inside a class).  So
    // define it here, then typedef it into TokenStream with static consts to
    // bring the initializers into scope.
    enum Modifier
    {
        // Normal operation.
        None,

        // Looking for an operand, not an operator.  In practice, this means
        // that when '/' is seen, we look for a regexp instead of just returning
        // Div.
        Operand,

        // Treat subsequent characters as the tail of a template literal, after
        // a template substitution, beginning with a "}", continuing with zero
        // or more template literal characters, and ending with either "${" or
        // the end of the template literal.  For example:
        //
        //   var entity = "world";
        //   var s = `Hello ${entity}!`;
        //                          ^ TemplateTail context
        TemplateTail,
    };
    enum ModifierException
    {
        NoException,

        // Used in following 2 cases:
        // a) After |yield| we look for a token on the same line that starts an
        // expression (Operand): |yield <expr>|.  If no token is found, the
        // |yield| stands alone, and the next token on a subsequent line must
        // be: a comma continuing a comma expression, a semicolon terminating
        // the statement that ended with |yield|, or the start of another
        // statement (possibly an expression statement).  The comma/semicolon
        // cases are gotten as operators (None), contrasting with Operand
        // earlier.
        // b) After an arrow function with a block body in an expression
        // statement, the next token must be: a colon in a conditional
        // expression, a comma continuing a comma expression, a semicolon
        // terminating the statement, or the token on a subsequent line that is
        // the start of another statement (possibly an expression statement).
        // Colon is gotten as operator (None), and it should only be gotten in
        // conditional expression and missing it results in SyntaxError.
        // Comma/semicolon cases are also gotten as operators (None), and 4th
        // case is gotten after them.  If no comma/semicolon found but EOL,
        // the next token should be gotten as operand in 4th case (especially if
        // '/' is the first character).  So we should peek the token as
        // operand before try getting colon/comma/semicolon.
        // See also the comment in Parser::assignExpr().
        NoneIsOperand,

        // If a semicolon is inserted automatically, the next token is already
        // gotten with None, but we expect Operand.
        OperandIsNone,
    };
    friend class TokenStreamShared;

  public:
    // WARNING: TokenStreamSpecific::Position assumes that the only GC things
    //          a Token includes are atoms.  DON'T ADD NON-ATOM GC THING
    //          POINTERS HERE UNLESS YOU ADD ADDITIONAL ROOTING TO THAT CLASS.

    TokenKind           type;           // char value or above enumerator
    TokenPos            pos;            // token position in file
    union {
      private:
        friend struct Token;
        PropertyName*   name;          // non-numeric atom
        JSAtom*         atom;          // potentially-numeric atom
        struct {
            double      value;          // floating point number
            DecimalPoint decimalPoint;  // literal contains '.'
        } number;
        RegExpFlag      reflags;        // regexp flags; use tokenbuf to access
                                        //   regexp chars
    } u;
#ifdef DEBUG
    Modifier modifier;                  // Modifier used to get this token
    ModifierException modifierException; // Exception for this modifier
#endif

    // Mutators

    void setName(PropertyName* name) {
        MOZ_ASSERT(type == TokenKind::Name);
        u.name = name;
    }

    void setAtom(JSAtom* atom) {
        MOZ_ASSERT(type == TokenKind::String ||
                   type == TokenKind::TemplateHead ||
                   type == TokenKind::NoSubsTemplate);
        u.atom = atom;
    }

    void setRegExpFlags(RegExpFlag flags) {
        MOZ_ASSERT(type == TokenKind::RegExp);
        MOZ_ASSERT((flags & AllFlags) == flags);
        u.reflags = flags;
    }

    void setNumber(double n, DecimalPoint decimalPoint) {
        MOZ_ASSERT(type == TokenKind::Number);
        u.number.value = n;
        u.number.decimalPoint = decimalPoint;
    }

    // Type-safe accessors

    PropertyName* name() const {
        MOZ_ASSERT(type == TokenKind::Name);
        return u.name->JSAtom::asPropertyName(); // poor-man's type verification
    }

    JSAtom* atom() const {
        MOZ_ASSERT(type == TokenKind::String ||
                   type == TokenKind::TemplateHead ||
                   type == TokenKind::NoSubsTemplate);
        return u.atom;
    }

    RegExpFlag regExpFlags() const {
        MOZ_ASSERT(type == TokenKind::RegExp);
        MOZ_ASSERT((u.reflags & AllFlags) == u.reflags);
        return u.reflags;
    }

    double number() const {
        MOZ_ASSERT(type == TokenKind::Number);
        return u.number.value;
    }

    DecimalPoint decimalPoint() const {
        MOZ_ASSERT(type == TokenKind::Number);
        return u.number.decimalPoint;
    }
};

extern TokenKind
ReservedWordTokenKind(PropertyName* str);

extern const char*
ReservedWordToCharZ(PropertyName* str);

extern const char*
ReservedWordToCharZ(TokenKind tt);

// Ideally, tokenizing would be entirely independent of context.  But the
// strict mode flag, which is in SharedContext, affects tokenizing, and
// TokenStream needs to see it.
//
// This class is a tiny back-channel from TokenStream to the strict mode flag
// that avoids exposing the rest of SharedContext to TokenStream.
//
class StrictModeGetter {
  public:
    virtual bool strictMode() = 0;
};

struct TokenStreamFlags
{
    bool isEOF:1;           // Hit end of file.
    bool isDirtyLine:1;     // Non-whitespace since start of line.
    bool sawOctalEscape:1;  // Saw an octal character escape.
    bool hadError:1;        // Hit a syntax error, at start or during a
                            // token.

    TokenStreamFlags()
      : isEOF(), isDirtyLine(), sawOctalEscape(), hadError()
    {}
};


/**
 * TokenStream types and constants that are used in both TokenStreamAnyChars
 * and TokenStreamSpecific.  Do not add any non-static data members to this
 * class!
 */
class TokenStreamShared
{
  protected:
    static constexpr size_t ntokens = 4; // 1 current + 2 lookahead, rounded
                                         // to power of 2 to avoid divmod by 3

    static constexpr unsigned ntokensMask = ntokens - 1;

  public:
    static constexpr unsigned maxLookahead = 2;

    static constexpr uint32_t NoOffset = UINT32_MAX;

    using Modifier = Token::Modifier;
    static constexpr Modifier None = Token::None;
    static constexpr Modifier Operand = Token::Operand;
    static constexpr Modifier TemplateTail = Token::TemplateTail;

    using ModifierException = Token::ModifierException;
    static constexpr ModifierException NoException = Token::NoException;
    static constexpr ModifierException NoneIsOperand = Token::NoneIsOperand;
    static constexpr ModifierException OperandIsNone = Token::OperandIsNone;

    static void
    verifyConsistentModifier(Modifier modifier, Token lookaheadToken)
    {
#ifdef DEBUG
        // Easy case: modifiers match.
        if (modifier == lookaheadToken.modifier)
            return;

        if (lookaheadToken.modifierException == OperandIsNone) {
            // getToken(Operand) permissibly following getToken().
            if (modifier == Operand && lookaheadToken.modifier == None)
                return;
        }

        if (lookaheadToken.modifierException == NoneIsOperand) {
            // getToken() permissibly following getToken(Operand).
            if (modifier == None && lookaheadToken.modifier == Operand)
                return;
        }

        MOZ_ASSERT_UNREACHABLE("this token was previously looked up with a "
                               "different modifier, potentially making "
                               "tokenization non-deterministic");
#endif
    }
};

static_assert(mozilla::IsEmpty<TokenStreamShared>::value,
              "TokenStreamShared shouldn't bloat classes that inherit from it");

template<typename CharT, class AnyCharsAccess>
class TokenStreamSpecific;

class TokenStreamAnyChars
  : public TokenStreamShared,
    public ErrorReporter
{
  public:
    TokenStreamAnyChars(JSContext* cx, const ReadOnlyCompileOptions& options,
                        StrictModeGetter* smg);

    template<typename CharT, class AnyCharsAccess> friend class GeneralTokenStreamChars;
    template<typename CharT, class AnyCharsAccess> friend class TokenStreamSpecific;

    // Accessors.
    const Token& currentToken() const { return tokens[cursor]; }
    bool isCurrentTokenType(TokenKind type) const {
        return currentToken().type == type;
    }

    bool getMutedErrors() const { return mutedErrors; }

    MOZ_MUST_USE bool checkOptions();

  private:
    PropertyName* reservedWordToPropertyName(TokenKind tt) const;

    void undoGetChar();

  public:
    PropertyName* currentName() const {
        if (isCurrentTokenType(TokenKind::Name))
            return currentToken().name();

        MOZ_ASSERT(TokenKindIsPossibleIdentifierName(currentToken().type));
        return reservedWordToPropertyName(currentToken().type);
    }

    bool currentNameHasEscapes() const {
        if (isCurrentTokenType(TokenKind::Name)) {
            TokenPos pos = currentToken().pos;
            return (pos.end - pos.begin) != currentToken().name()->length();
        }

        MOZ_ASSERT(TokenKindIsPossibleIdentifierName(currentToken().type));
        return false;
    }

    PropertyName* nextName() const {
        if (nextToken().type != TokenKind::Name)
            return nextToken().name();

        MOZ_ASSERT(TokenKindIsPossibleIdentifierName(nextToken().type));
        return reservedWordToPropertyName(nextToken().type);
    }

    bool isCurrentTokenAssignment() const {
        return TokenKindIsAssignment(currentToken().type);
    }

    // Flag methods.
    bool isEOF() const { return flags.isEOF; }
    bool sawOctalEscape() const { return flags.sawOctalEscape; }
    bool hadError() const { return flags.hadError; }
    void clearSawOctalEscape() { flags.sawOctalEscape = false; }

    bool hasInvalidTemplateEscape() const {
        return invalidTemplateEscapeType != InvalidEscapeType::None;
    }
    void clearInvalidTemplateEscape() {
        invalidTemplateEscapeType = InvalidEscapeType::None;
    }

  private:
    // This is private because it should only be called by the tokenizer while
    // tokenizing not by, for example, BytecodeEmitter.
    bool strictMode() const { return strictModeGetter && strictModeGetter->strictMode(); }

    void setInvalidTemplateEscape(uint32_t offset, InvalidEscapeType type) {
        MOZ_ASSERT(type != InvalidEscapeType::None);
        if (invalidTemplateEscapeType != InvalidEscapeType::None)
            return;
        invalidTemplateEscapeOffset = offset;
        invalidTemplateEscapeType = type;
    }

    uint32_t invalidTemplateEscapeOffset = 0;
    InvalidEscapeType invalidTemplateEscapeType = InvalidEscapeType::None;

  public:
    void addModifierException(ModifierException modifierException) {
#ifdef DEBUG
        const Token& next = nextToken();

        // Permit adding the same exception multiple times.  This is important
        // particularly for Parser::assignExpr's early fast-path cases and
        // arrow function parsing: we want to add modifier exceptions in the
        // fast paths, then potentially (but not necessarily) duplicate them
        // after parsing all of an arrow function.
        if (next.modifierException == modifierException)
            return;

        if (next.modifierException == NoneIsOperand) {
            // Token after yield expression without operand already has
            // NoneIsOperand exception.
            MOZ_ASSERT(modifierException == OperandIsNone);
            MOZ_ASSERT(next.type != TokenKind::Div,
                       "next token requires contextual specifier to be parsed unambiguously");

            // Do not update modifierException.
            return;
        }

        MOZ_ASSERT(next.modifierException == NoException);
        switch (modifierException) {
          case NoneIsOperand:
            MOZ_ASSERT(next.modifier == Operand);
            MOZ_ASSERT(next.type != TokenKind::Div,
                       "next token requires contextual specifier to be parsed unambiguously");
            break;
          case OperandIsNone:
            MOZ_ASSERT(next.modifier == None);
            MOZ_ASSERT(next.type != TokenKind::Div && next.type != TokenKind::RegExp,
                       "next token requires contextual specifier to be parsed unambiguously");
            break;
          default:
            MOZ_CRASH("unexpected modifier exception");
        }
        tokens[(cursor + 1) & ntokensMask].modifierException = modifierException;
#endif
    }

#ifdef DEBUG
    inline bool debugHasNoLookahead() const {
        return lookahead == 0;
    }
#endif

    bool hasDisplayURL() const {
        return displayURL_ != nullptr;
    }

    char16_t* displayURL() {
        return displayURL_.get();
    }

    bool hasSourceMapURL() const {
        return sourceMapURL_ != nullptr;
    }

    char16_t* sourceMapURL() {
        return sourceMapURL_.get();
    }

    // This class maps a userbuf offset (which is 0-indexed) to a line number
    // (which is 1-indexed) and a column index (which is 0-indexed).
    class SourceCoords
    {
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
        // To convert a "line number" to a "line index" (i.e. an index into
        // |lineStartOffsets_|), subtract |initialLineNum_|.  E.g. line 3's
        // line index is (3 - initialLineNum_), which is 2.  Therefore
        // lineStartOffsets_[2] holds the buffer offset for the start of line 3,
        // which is 14.  (Note that |initialLineNum_| is often 1, but not
        // always.)
        //
        // The first element is always initialLineOffset, passed to the
        // constructor, and the last element is always the MAX_PTR sentinel.
        //
        // offset-to-line/column lookups are O(log n) in the worst case (binary
        // search), but in practice they're heavily clustered and we do better
        // than that by using the previous lookup's result (lastLineIndex_) as
        // a starting point.
        //
        // Checking if an offset lies within a particular line number
        // (isOnThisLine()) is O(1).
        //
        Vector<uint32_t, 128> lineStartOffsets_;
        uint32_t            initialLineNum_;
        uint32_t            initialColumn_;

        // This is mutable because it's modified on every search, but that fact
        // isn't visible outside this class.
        mutable uint32_t    lastLineIndex_;

        uint32_t lineIndexOf(uint32_t offset) const;

        static const uint32_t MAX_PTR = UINT32_MAX;

        uint32_t lineIndexToNum(uint32_t lineIndex) const { return lineIndex + initialLineNum_; }
        uint32_t lineNumToIndex(uint32_t lineNum)   const { return lineNum   - initialLineNum_; }
        uint32_t lineIndexAndOffsetToColumn(uint32_t lineIndex, uint32_t offset) const {
            uint32_t lineStartOffset = lineStartOffsets_[lineIndex];
            MOZ_RELEASE_ASSERT(offset >= lineStartOffset);
            uint32_t column = offset - lineStartOffset;
            if (lineIndex == 0)
                return column + initialColumn_;
            return column;
        }

      public:
        SourceCoords(JSContext* cx, uint32_t ln, uint32_t col, uint32_t initialLineOffset);

        MOZ_MUST_USE bool add(uint32_t lineNum, uint32_t lineStartOffset);
        MOZ_MUST_USE bool fill(const SourceCoords& other);

        bool isOnThisLine(uint32_t offset, uint32_t lineNum, bool* onThisLine) const {
            uint32_t lineIndex = lineNumToIndex(lineNum);
            if (lineIndex + 1 >= lineStartOffsets_.length()) // +1 due to sentinel
                return false;
            *onThisLine = lineStartOffsets_[lineIndex] <= offset &&
                          offset < lineStartOffsets_[lineIndex + 1];
            return true;
        }

        uint32_t lineNum(uint32_t offset) const;
        uint32_t columnIndex(uint32_t offset) const;
        void lineNumAndColumnIndex(uint32_t offset, uint32_t* lineNum, uint32_t* column) const;
    };

    SourceCoords srcCoords;

    JSAtomState& names() const {
        return cx->names();
    }

    JSContext* context() const {
        return cx;
    }

    /**
     * Fill in |err|, excepting line-of-context-related fields.  If the token
     * stream has location information, use that and return true.  If it does
     * not, use the caller's location information and return false.
     */
    bool fillExcludingContext(ErrorMetadata* err, uint32_t offset);

    void updateFlagsForEOL();

  private:
    MOZ_MUST_USE MOZ_ALWAYS_INLINE bool internalUpdateLineInfoForEOL(uint32_t lineStartOffset);

  public:
    const Token& nextToken() const {
        MOZ_ASSERT(hasLookahead());
        return tokens[(cursor + 1) & ntokensMask];
    }

    bool hasLookahead() const { return lookahead > 0; }

    // Push the last scanned token back into the stream.
    void ungetToken() {
        MOZ_ASSERT(lookahead < maxLookahead);
        lookahead++;
        cursor = (cursor - 1) & ntokensMask;
    }

  public:
    MOZ_MUST_USE bool compileWarning(ErrorMetadata&& metadata, UniquePtr<JSErrorNotes> notes,
                                     unsigned flags, unsigned errorNumber, va_list args);

    // Compute error metadata for an error at no offset.
    void computeErrorMetadataNoOffset(ErrorMetadata* err);

  public:
    // ErrorReporter API.

    const JS::ReadOnlyCompileOptions& options() const final {
        return options_;
    }

    void
    lineAndColumnAt(size_t offset, uint32_t* line, uint32_t* column) const final;

    void currentLineAndColumn(uint32_t* line, uint32_t* column) const final;

    bool hasTokenizationStarted() const final;
    void reportErrorNoOffsetVA(unsigned errorNumber, va_list args) final;

    const char* getFilename() const final {
        return filename_;
    }

  protected:
    // Options used for parsing/tokenizing.
    const ReadOnlyCompileOptions& options_;

    Token               tokens[ntokens];    // circular token buffer
    unsigned            cursor;             // index of last parsed token
    unsigned            lookahead;          // count of lookahead tokens
    unsigned            lineno;             // current line number
    TokenStreamFlags    flags;              // flags -- see above
    size_t              linebase;           // start of current line
    size_t              prevLinebase;       // start of previous line;  size_t(-1) if on the first line
    const char*         filename_;          // input filename or null
    UniqueTwoByteChars  displayURL_;        // the user's requested source URL or null
    UniqueTwoByteChars  sourceMapURL_;      // source map's filename or null
    uint8_t             isExprEnding[size_t(TokenKind::Limit)];// which tokens definitely terminate exprs?
    JSContext* const    cx;
    bool                mutedErrors;
    StrictModeGetter*   strictModeGetter;  // used to test for strict mode
};

template<typename CharT>
class TokenStreamCharsBase
{
  protected:
    void ungetCharIgnoreEOL(int32_t c);

  public:
    using CharBuffer = Vector<CharT, 32>;

    TokenStreamCharsBase(JSContext* cx, const CharT* chars, size_t length, size_t startOffset);

    static MOZ_ALWAYS_INLINE JSAtom*
    atomizeChars(JSContext* cx, const CharT* chars, size_t length);

    const CharBuffer& getTokenbuf() const { return tokenbuf; }

    MOZ_MUST_USE bool copyTokenbufTo(JSContext* cx,
                                     UniquePtr<char16_t[], JS::FreePolicy>* destination);

    // This is the low-level interface to the JS source code buffer.  It just
    // gets raw chars, basically.  TokenStreams functions are layered on top
    // and do some extra stuff like converting all EOL sequences to '\n',
    // tracking the line number, and setting |flags.isEOF|.  (The "raw" in "raw
    // chars" refers to the lack of EOL sequence normalization.)
    //
    // buf[0..length-1] often represents a substring of some larger source,
    // where we have only the substring in memory. The |startOffset| argument
    // indicates the offset within this larger string at which our string
    // begins, the offset of |buf[0]|.
    class TokenBuf
    {
      public:
        TokenBuf(const CharT* buf, size_t length, size_t startOffset)
          : base_(buf),
            startOffset_(startOffset),
            limit_(buf + length),
            ptr(buf)
        { }

        bool hasRawChars() const {
            return ptr < limit_;
        }

        bool atStart() const {
            return offset() == 0;
        }

        size_t startOffset() const {
            return startOffset_;
        }

        size_t offset() const {
            return startOffset_ + mozilla::PointerRangeSize(base_, ptr);
        }

        const CharT* rawCharPtrAt(size_t offset) const {
            MOZ_ASSERT(startOffset_ <= offset);
            MOZ_ASSERT(offset - startOffset_ <= mozilla::PointerRangeSize(base_, limit_));
            return base_ + (offset - startOffset_);
        }

        const CharT* limit() const {
            return limit_;
        }

        CharT getRawChar() {
            return *ptr++;      // this will nullptr-crash if poisoned
        }

        CharT peekRawChar() const {
            return *ptr;        // this will nullptr-crash if poisoned
        }

        bool matchRawChar(CharT c) {
            if (*ptr == c) {    // this will nullptr-crash if poisoned
                ptr++;
                return true;
            }
            return false;
        }

        bool matchRawCharBackwards(CharT c) {
            MOZ_ASSERT(ptr);     // make sure it hasn't been poisoned
            if (*(ptr - 1) == c) {
                ptr--;
                return true;
            }
            return false;
        }

        void ungetRawChar() {
            MOZ_ASSERT(ptr);     // make sure it hasn't been poisoned
            ptr--;
        }

        const CharT* addressOfNextRawChar(bool allowPoisoned = false) const {
            MOZ_ASSERT_IF(!allowPoisoned, ptr);     // make sure it hasn't been poisoned
            return ptr;
        }

        // Use this with caution!
        void setAddressOfNextRawChar(const CharT* a, bool allowPoisoned = false) {
            MOZ_ASSERT_IF(!allowPoisoned, a);
            ptr = a;
        }

#ifdef DEBUG
        // Poison the TokenBuf so it cannot be accessed again.
        void poison() {
            ptr = nullptr;
        }
#endif

        static bool isRawEOLChar(int32_t c) {
            return c == '\n' ||
                   c == '\r' ||
                   c == unicode::LINE_SEPARATOR ||
                   c == unicode::PARA_SEPARATOR;
        }

        // Returns the offset of the next EOL, but stops once 'max' characters
        // have been scanned (*including* the char at startOffset_).
        size_t findEOLMax(size_t start, size_t max);

      private:
        /** Base of buffer. */
        const CharT* base_;

        /** Offset of base_[0]. */
        uint32_t startOffset_;

        /** Limit for quick bounds check. */
        const CharT* limit_;

        /** Next char to get. */
        const CharT* ptr;
    };

    MOZ_MUST_USE bool appendCodePointToTokenbuf(uint32_t codePoint);

    class MOZ_STACK_CLASS Position
    {
      public:
        // The JS_HAZ_ROOTED is permissible below because: 1) the only field in
        // Position that can keep GC things alive is Token, 2) the only GC
        // things Token can keep alive are atoms, and 3) the AutoKeepAtoms&
        // passed to the constructor here represents that collection of atoms
        // is disabled while atoms in Tokens in this Position are alive.  DON'T
        // ADD NON-ATOM GC THING POINTERS HERE!  They would create a rooting
        // hazard that JS_HAZ_ROOTED will cause to be ignored.
        explicit Position(AutoKeepAtoms&) { }

      private:
        Position(const Position&) = delete;

        // Technically this should only friend TokenStreamSpecific instantiated
        // with CharT (letting the AnyCharsAccess parameter vary), but C++
        // doesn't allow partial friend specialization.
        template<typename, class> friend class TokenStreamSpecific;

        const CharT* buf;
        TokenStreamFlags flags;
        unsigned lineno;
        size_t linebase;
        size_t prevLinebase;
        Token currentToken;
        unsigned lookahead;
        Token lookaheadTokens[TokenStreamShared::maxLookahead];
    } JS_HAZ_ROOTED;

  protected:
    /** User input buffer. */
    TokenBuf userbuf;

    /** Current token string buffer. */
    CharBuffer tokenbuf;
};

template<>
/* static */ MOZ_ALWAYS_INLINE JSAtom*
TokenStreamCharsBase<char16_t>::atomizeChars(JSContext* cx, const char16_t* chars, size_t length)
{
    return AtomizeChars(cx, chars, length);
}

template<typename CharT, class AnyCharsAccess>
class GeneralTokenStreamChars
  : public TokenStreamCharsBase<CharT>
{
    using CharsSharedBase = TokenStreamCharsBase<CharT>;

  protected:
    using typename CharsSharedBase::TokenBuf;

    using CharsSharedBase::userbuf;

  public:
    using CharsSharedBase::CharsSharedBase;

    TokenStreamAnyChars& anyCharsAccess() {
        return AnyCharsAccess::anyChars(this);
    }

    const TokenStreamAnyChars& anyCharsAccess() const {
        return AnyCharsAccess::anyChars(this);
    }

    using TokenStreamSpecific = frontend::TokenStreamSpecific<CharT, AnyCharsAccess>;

    TokenStreamSpecific* asSpecific() {
        static_assert(mozilla::IsBaseOf<GeneralTokenStreamChars, TokenStreamSpecific>::value,
                      "static_cast below presumes an inheritance relationship");

        return static_cast<TokenStreamSpecific*>(this);
    }

    int32_t getCharIgnoreEOL();

    void ungetChar(int32_t c);
};

template<typename CharT, class AnyCharsAccess> class TokenStreamChars;

template<class AnyCharsAccess>
class TokenStreamChars<char16_t, AnyCharsAccess>
  : public GeneralTokenStreamChars<char16_t, AnyCharsAccess>
{
  private:
    using Self = TokenStreamChars<char16_t, AnyCharsAccess>;
    using GeneralCharsBase = GeneralTokenStreamChars<char16_t, AnyCharsAccess>;
    using CharsSharedBase = TokenStreamCharsBase<char16_t>;

    using GeneralCharsBase::asSpecific;

    using typename GeneralCharsBase::TokenStreamSpecific;

    void matchMultiUnitCodePointSlow(char16_t lead, uint32_t* codePoint);

  protected:
    using GeneralCharsBase::anyCharsAccess;
    using GeneralCharsBase::getCharIgnoreEOL;
    using CharsSharedBase::ungetCharIgnoreEOL;
    using GeneralCharsBase::userbuf;

    using GeneralCharsBase::GeneralCharsBase;

    // |c| must be the code unit just gotten.  If it and the subsequent code
    // unit form a valid surrogate pair, get the second code unit, set
    // |*codePoint| to the code point encoded by the surrogate pair, and return
    // true.  Otherwise do not get a second code unit, set |*codePoint = 0|,
    // and return true.
    //
    // ECMAScript specifically requires that unpaired UTF-16 surrogates be
    // treated as the corresponding code point and not as an error.  See
    // <https://tc39.github.io/ecma262/#sec-ecmascript-language-types-string-type>.
    // Therefore this function always returns true.  The |bool| return type
    // exists so that a future UTF-8 |TokenStreamChars| can treat malformed
    // multi-code unit UTF-8 sequences as errors.  (Because ECMAScript only
    // interprets UTF-16 inputs, the process of translating the UTF-8 to UTF-16
    // would fail, so no script should execute.  Technically, we shouldn't even
    // be tokenizing -- but it probably isn't realistic to assume every user
    // correctly passes only valid UTF-8, at least not without better types in
    // our codebase for strings that by construction only contain valid UTF-8.)
    MOZ_ALWAYS_INLINE bool matchMultiUnitCodePoint(char16_t c, uint32_t* codePoint) {
        if (MOZ_LIKELY(!unicode::IsLeadSurrogate(c)))
            *codePoint = 0;
        else
            matchMultiUnitCodePointSlow(c, codePoint);
        return true;
    }

    void ungetCodePointIgnoreEOL(uint32_t codePoint);
};

// TokenStream is the lexical scanner for JavaScript source text.
//
// It takes a buffer of CharT characters (currently only char16_t encoding
// UTF-16, but we're adding either UTF-8 or Latin-1 single-byte text soon) and
// linearly scans it into |Token|s.
//
// Internally the class uses a four element circular buffer |tokens| of
// |Token|s. As an index for |tokens|, the member |cursor| points to the
// current token. Calls to getToken() increase |cursor| by one and return the
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
// The methods seek() and tell() allow to rescan from a previous visited
// location of the buffer.
//
template<typename CharT, class AnyCharsAccess>
class MOZ_STACK_CLASS TokenStreamSpecific
  : public TokenStreamChars<CharT, AnyCharsAccess>,
    public TokenStreamShared
{
  public:
    using CharsBase = TokenStreamChars<CharT, AnyCharsAccess>;
    using GeneralCharsBase = GeneralTokenStreamChars<CharT, AnyCharsAccess>;
    using CharsSharedBase = TokenStreamCharsBase<CharT>;

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
    using typename CharsSharedBase::Position;

  public:
    using GeneralCharsBase::anyCharsAccess;
    using CharsSharedBase::getTokenbuf;

  private:
    using typename CharsSharedBase::CharBuffer;
    using typename CharsSharedBase::TokenBuf;

  private:
    using CharsSharedBase::appendCodePointToTokenbuf;
    using CharsSharedBase::atomizeChars;
    using CharsSharedBase::copyTokenbufTo;
    using GeneralCharsBase::getCharIgnoreEOL;
    using CharsBase::matchMultiUnitCodePoint;
    using CharsSharedBase::tokenbuf;
    using GeneralCharsBase::ungetChar;
    using CharsSharedBase::ungetCharIgnoreEOL;
    using CharsBase::ungetCodePointIgnoreEOL;
    using CharsSharedBase::userbuf;

  public:
    TokenStreamSpecific(JSContext* cx, const ReadOnlyCompileOptions& options,
                        const CharT* base, size_t length);

    // If there is an invalid escape in a template, report it and return false,
    // otherwise return true.
    bool checkForInvalidTemplateEscapeError() {
        if (anyCharsAccess().invalidTemplateEscapeType == InvalidEscapeType::None)
            return true;

        reportInvalidEscapeError(anyCharsAccess().invalidTemplateEscapeOffset,
                                 anyCharsAccess().invalidTemplateEscapeType);
        return false;
    }

    // TokenStream-specific error reporters.
    void reportError(unsigned errorNumber, ...);

    // Report the given error at the current offset.
    void error(unsigned errorNumber, ...);

    // Report the given error at the given offset.
    void errorAt(uint32_t offset, unsigned errorNumber, ...);

    // Warn at the current offset.
    MOZ_MUST_USE bool warning(unsigned errorNumber, ...);

  private:
    // Compute a line of context for an otherwise-filled-in |err| at the given
    // offset in this token stream.  (This function basically exists to make
    // |computeErrorMetadata| more readable and shouldn't be called elsewhere.)
    MOZ_MUST_USE bool computeLineOfContext(ErrorMetadata* err, uint32_t offset);

  public:
    // Compute error metadata for an error at the given offset.
    MOZ_MUST_USE bool computeErrorMetadata(ErrorMetadata* err, uint32_t offset);

    // General-purpose error reporters.  You should avoid calling these
    // directly, and instead use the more succinct alternatives (error(),
    // warning(), &c.) in TokenStream, Parser, and BytecodeEmitter.
    //
    // These functions take a |va_list*| parameter, not a |va_list| parameter,
    // to hack around bug 1363116.  (Longer-term, the right fix is of course to
    // not use ellipsis functions or |va_list| at all in error reporting.)
    bool reportStrictModeErrorNumberVA(UniquePtr<JSErrorNotes> notes, uint32_t offset,
                                       bool strictMode, unsigned errorNumber, va_list* args);
    bool reportExtraWarningErrorNumberVA(UniquePtr<JSErrorNotes> notes, uint32_t offset,
                                         unsigned errorNumber, va_list* args);

    JSAtom* getRawTemplateStringAtom() {
        TokenStreamAnyChars& anyChars = anyCharsAccess();

        MOZ_ASSERT(anyChars.currentToken().type == TokenKind::TemplateHead ||
                   anyChars.currentToken().type == TokenKind::NoSubsTemplate);
        const CharT* cur = userbuf.rawCharPtrAt(anyChars.currentToken().pos.begin + 1);
        const CharT* end;
        if (anyChars.currentToken().type == TokenKind::TemplateHead) {
            // Of the form    |`...${|   or   |}...${|
            end = userbuf.rawCharPtrAt(anyChars.currentToken().pos.end - 2);
        } else {
            // NO_SUBS_TEMPLATE is of the form   |`...`|   or   |}...`|
            end = userbuf.rawCharPtrAt(anyChars.currentToken().pos.end - 1);
        }

        CharBuffer charbuf(anyChars.cx);
        while (cur < end) {
            CharT ch = *cur;
            if (ch == '\r') {
                ch = '\n';
                if ((cur + 1 < end) && (*(cur + 1) == '\n'))
                    cur++;
            }
            if (!charbuf.append(ch))
                return nullptr;
            cur++;
        }
        return atomizeChars(anyChars.cx, charbuf.begin(), charbuf.length());
    }

  private:
    // This is private because it should only be called by the tokenizer while
    // tokenizing not by, for example, BytecodeEmitter.
    bool reportStrictModeError(unsigned errorNumber, ...);

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
                errorAt(offset, JSMSG_DEPRECATED_OCTAL);
                return;
        }
    }

    MOZ_MUST_USE bool putIdentInTokenbuf(const CharT* identStart);

  public:
    // Advance to the next token.  If the token stream encountered an error,
    // return false.  Otherwise return true and store the token kind in |*ttp|.
    MOZ_MUST_USE bool getToken(TokenKind* ttp, Modifier modifier = None) {
        // Check for a pushed-back token resulting from mismatching lookahead.
        TokenStreamAnyChars& anyChars = anyCharsAccess();
        if (anyChars.lookahead != 0) {
            MOZ_ASSERT(!anyChars.flags.hadError);
            anyChars.lookahead--;
            anyChars.cursor = (anyChars.cursor + 1) & ntokensMask;
            TokenKind tt = anyChars.currentToken().type;
            MOZ_ASSERT(tt != TokenKind::Eol);
            verifyConsistentModifier(modifier, anyChars.currentToken());
            *ttp = tt;
            return true;
        }

        return getTokenInternal(ttp, modifier);
    }

    MOZ_MUST_USE bool peekToken(TokenKind* ttp, Modifier modifier = None) {
        TokenStreamAnyChars& anyChars = anyCharsAccess();
        if (anyChars.lookahead > 0) {
            MOZ_ASSERT(!anyChars.flags.hadError);
            verifyConsistentModifier(modifier, anyChars.nextToken());
            *ttp = anyChars.nextToken().type;
            return true;
        }
        if (!getTokenInternal(ttp, modifier))
            return false;
        anyChars.ungetToken();
        return true;
    }

    MOZ_MUST_USE bool peekTokenPos(TokenPos* posp, Modifier modifier = None) {
        TokenStreamAnyChars& anyChars = anyCharsAccess();
        if (anyChars.lookahead == 0) {
            TokenKind tt;
            if (!getTokenInternal(&tt, modifier))
                return false;
            anyChars.ungetToken();
            MOZ_ASSERT(anyChars.hasLookahead());
        } else {
            MOZ_ASSERT(!anyChars.flags.hadError);
            verifyConsistentModifier(modifier, anyChars.nextToken());
        }
        *posp = anyChars.nextToken().pos;
        return true;
    }

    MOZ_MUST_USE bool peekOffset(uint32_t* offset, Modifier modifier = None) {
        TokenPos pos;
        if (!peekTokenPos(&pos, modifier))
            return false;
        *offset = pos.begin;
        return true;
    }

    // This is like peekToken(), with one exception:  if there is an EOL
    // between the end of the current token and the start of the next token, it
    // return true and store Eol in |*ttp|.  In that case, no token with
    // Eol is actually created, just a Eol TokenKind is returned, and
    // currentToken() shouldn't be consulted.  (This is the only place Eol
    // is produced.)
    MOZ_ALWAYS_INLINE MOZ_MUST_USE bool
    peekTokenSameLine(TokenKind* ttp, Modifier modifier = None) {
        TokenStreamAnyChars& anyChars = anyCharsAccess();
        const Token& curr = anyChars.currentToken();

        // If lookahead != 0, we have scanned ahead at least one token, and
        // |lineno| is the line that the furthest-scanned token ends on.  If
        // it's the same as the line that the current token ends on, that's a
        // stronger condition than what we are looking for, and we don't need
        // to return Eol.
        if (anyChars.lookahead != 0) {
            bool onThisLine;
            if (!anyChars.srcCoords.isOnThisLine(curr.pos.end, anyChars.lineno, &onThisLine)) {
                reportError(JSMSG_OUT_OF_MEMORY);
                return false;
            }

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
        if (!getToken(&tmp, modifier))
            return false;
        const Token& next = anyChars.currentToken();
        anyChars.ungetToken();

        const auto& srcCoords = anyChars.srcCoords;
        *ttp = srcCoords.lineNum(curr.pos.end) == srcCoords.lineNum(next.pos.begin)
             ? next.type
             : TokenKind::Eol;
        return true;
    }

    // Get the next token from the stream if its kind is |tt|.
    MOZ_MUST_USE bool matchToken(bool* matchedp, TokenKind tt, Modifier modifier = None) {
        TokenKind token;
        if (!getToken(&token, modifier))
            return false;
        if (token == tt) {
            *matchedp = true;
        } else {
            anyCharsAccess().ungetToken();
            *matchedp = false;
        }
        return true;
    }

    void consumeKnownToken(TokenKind tt, Modifier modifier = None) {
        bool matched;
        MOZ_ASSERT(anyCharsAccess().hasLookahead());
        MOZ_ALWAYS_TRUE(matchToken(&matched, tt, modifier));
        MOZ_ALWAYS_TRUE(matched);
    }

    MOZ_MUST_USE bool nextTokenEndsExpr(bool* endsExpr) {
        TokenKind tt;
        if (!peekToken(&tt))
            return false;

        *endsExpr = anyCharsAccess().isExprEnding[size_t(tt)];
        if (*endsExpr) {
            // If the next token ends an overall Expression, we'll parse this
            // Expression without ever invoking Parser::orExpr().  But we need
            // that function's side effect of adding this modifier exception,
            // so we have to do it manually here.
            anyCharsAccess().addModifierException(OperandIsNone);
        }
        return true;
    }

    MOZ_MUST_USE bool advance(size_t position);

    void tell(Position*);
    void seek(const Position& pos);
    MOZ_MUST_USE bool seek(const Position& pos, const TokenStreamAnyChars& other);

    const CharT* rawCharPtrAt(size_t offset) const {
        return userbuf.rawCharPtrAt(offset);
    }

    const CharT* rawLimit() const {
        return userbuf.limit();
    }

    MOZ_MUST_USE bool getTokenInternal(TokenKind* ttp, Modifier modifier);

    MOZ_MUST_USE bool getStringOrTemplateToken(char untilChar, Token** tp);

    // Try to get the next character, normalizing '\r', '\r\n', and '\n' into
    // '\n'.  Also updates internal line-counter state.  Return true on success
    // and store the character in |*c|.  Return false and leave |*c| undefined
    // on failure.
    MOZ_MUST_USE bool getChar(int32_t* cp);

    Token* newToken(ptrdiff_t adjust);
    uint32_t peekUnicodeEscape(uint32_t* codePoint);
    uint32_t peekExtendedUnicodeEscape(uint32_t* codePoint);
    uint32_t matchUnicodeEscapeIdStart(uint32_t* codePoint);
    bool matchUnicodeEscapeIdent(uint32_t* codePoint);
    bool peekChars(int n, CharT* cp);

    MOZ_MUST_USE bool getDirectives(bool isMultiline, bool shouldWarnDeprecated);
    MOZ_MUST_USE bool getDirective(bool isMultiline, bool shouldWarnDeprecated,
                                   const char* directive, uint8_t directiveLength,
                                   const char* errorMsgPragma,
                                   UniquePtr<char16_t[], JS::FreePolicy>* destination);
    MOZ_MUST_USE bool getDisplayURL(bool isMultiline, bool shouldWarnDeprecated);
    MOZ_MUST_USE bool getSourceMappingURL(bool isMultiline, bool shouldWarnDeprecated);

    // |expect| cannot be an EOL char.
    bool matchChar(int32_t expect) {
        MOZ_ASSERT(!TokenBuf::isRawEOLChar(expect));
        return MOZ_LIKELY(userbuf.hasRawChars()) && userbuf.matchRawChar(expect);
    }

    void consumeKnownChar(int32_t expect) {
        int32_t c;
        MOZ_ALWAYS_TRUE(getChar(&c));
        MOZ_ASSERT(c == expect);
    }

    MOZ_MUST_USE bool peekChar(int32_t* c) {
        if (!getChar(c))
            return false;
        ungetChar(*c);
        return true;
    }

    void skipChars(uint32_t n) {
        while (n-- > 0) {
            MOZ_ASSERT(userbuf.hasRawChars());
            mozilla::DebugOnly<int32_t> c = getCharIgnoreEOL();
            MOZ_ASSERT(c != '\n');
        }
    }

    void skipCharsIgnoreEOL(uint8_t n) {
        while (n-- > 0) {
            MOZ_ASSERT(userbuf.hasRawChars());
            getCharIgnoreEOL();
        }
    }

    MOZ_MUST_USE MOZ_ALWAYS_INLINE bool updateLineInfoForEOL();
};

class TokenStreamAnyCharsAccess
{
  public:
    template<class TokenStreamSpecific>
    static inline TokenStreamAnyChars& anyChars(TokenStreamSpecific* tss);

    template<class TokenStreamSpecific>
    static inline const TokenStreamAnyChars& anyChars(const TokenStreamSpecific* tss);
};

class MOZ_STACK_CLASS TokenStream final
  : public TokenStreamAnyChars,
    public TokenStreamSpecific<char16_t, TokenStreamAnyCharsAccess>
{
    using CharT = char16_t;

  public:
    TokenStream(JSContext* cx, const ReadOnlyCompileOptions& options,
                const CharT* base, size_t length, StrictModeGetter* smg)
    : TokenStreamAnyChars(cx, options, smg),
      TokenStreamSpecific<CharT, TokenStreamAnyCharsAccess>(cx, options, base, length)
    {}
};

template<class TokenStreamSpecific>
/* static */ inline TokenStreamAnyChars&
TokenStreamAnyCharsAccess::anyChars(TokenStreamSpecific* tss)
{
    auto* ts = static_cast<TokenStream*>(tss);
    return *static_cast<TokenStreamAnyChars*>(ts);
}

template<class TokenStreamSpecific>
/* static */ inline const TokenStreamAnyChars&
TokenStreamAnyCharsAccess::anyChars(const TokenStreamSpecific* tss)
{
    const auto* ts = static_cast<const TokenStream*>(tss);
    return *static_cast<const TokenStreamAnyChars*>(ts);
}

extern const char*
TokenKindToDesc(TokenKind tt);

} // namespace frontend
} // namespace js

extern JS_FRIEND_API(int)
js_fgets(char* buf, int size, FILE* file);

#ifdef DEBUG
extern const char*
TokenKindToString(js::frontend::TokenKind tt);
#endif

#endif /* frontend_TokenStream_h */
