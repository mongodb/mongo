/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TokenStream_h
#define frontend_TokenStream_h

// JS lexical scanner interface.

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "jscntxt.h"
#include "jspubtd.h"

#include "frontend/TokenKind.h"
#include "js/Vector.h"
#include "vm/RegExpObject.h"

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

struct Token
{
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

    // This constructor is necessary only for MSVC 2013 and how it compiles the
    // initialization of TokenStream::tokens.  That field is initialized as
    // tokens() in the constructor init-list.  This *should* zero the entire
    // array, then (because Token has a non-trivial constructor, because
    // TokenPos has a user-provided constructor) call the implicit Token
    // constructor on each element, which would call the TokenPos constructor
    // for Token::pos and do nothing.  (All of which is equivalent to just
    // zeroing TokenStream::tokens.)  But MSVC 2013 (2010/2012 don't have this
    // bug) doesn't zero out each element, so we need this extra constructor to
    // make it do the right thing.  (Token is used primarily by reference or
    // pointer, and it's only initialized a very few places, so having a
    // user-defined constructor won't hurt perf.)  See also bug 920318.
    Token()
      : pos(0, 0)
    {
        MOZ_MAKE_MEM_UNDEFINED(&type, sizeof(type));
    }

    // Mutators

    void setName(PropertyName* name) {
        MOZ_ASSERT(type == TOK_NAME);
        MOZ_ASSERT(!IsPoisonedPtr(name));
        u.name = name;
    }

    void setAtom(JSAtom* atom) {
        MOZ_ASSERT(type == TOK_STRING ||
                   type == TOK_TEMPLATE_HEAD ||
                   type == TOK_NO_SUBS_TEMPLATE);
        MOZ_ASSERT(!IsPoisonedPtr(atom));
        u.atom = atom;
    }

    void setRegExpFlags(js::RegExpFlag flags) {
        MOZ_ASSERT(type == TOK_REGEXP);
        MOZ_ASSERT((flags & AllFlags) == flags);
        u.reflags = flags;
    }

    void setNumber(double n, DecimalPoint decimalPoint) {
        MOZ_ASSERT(type == TOK_NUMBER);
        u.number.value = n;
        u.number.decimalPoint = decimalPoint;
    }

    // Type-safe accessors

    PropertyName* name() const {
        MOZ_ASSERT(type == TOK_NAME);
        return u.name->asPropertyName(); // poor-man's type verification
    }

    JSAtom* atom() const {
        MOZ_ASSERT(type == TOK_STRING ||
                   type == TOK_TEMPLATE_HEAD ||
                   type == TOK_NO_SUBS_TEMPLATE);
        return u.atom;
    }

    js::RegExpFlag regExpFlags() const {
        MOZ_ASSERT(type == TOK_REGEXP);
        MOZ_ASSERT((u.reflags & AllFlags) == u.reflags);
        return u.reflags;
    }

    double number() const {
        MOZ_ASSERT(type == TOK_NUMBER);
        return u.number.value;
    }

    DecimalPoint decimalPoint() const {
        MOZ_ASSERT(type == TOK_NUMBER);
        return u.number.decimalPoint;
    }
};

struct CompileError {
    JSErrorReport report;
    char* message;
    ErrorArgumentsType argumentsType;
    CompileError() : message(nullptr), argumentsType(ArgumentsAreUnicode) {}
    ~CompileError();
    void throwError(JSContext* cx);

  private:
    // CompileError owns raw allocated memory, so disable assignment and copying
    // for safety.
    void operator=(const CompileError&) = delete;
    CompileError(const CompileError&) = delete;
};

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

// TokenStream is the lexical scanner for Javascript source text.
//
// It takes a buffer of char16_t characters and linearly scans it into |Token|s.
// Internally the class uses a four element circular buffer |tokens| of
// |Token|s. As an index for |tokens|, the member |cursor| points to the
// current token.
// Calls to getToken() increase |cursor| by one and return the new current
// token. If a TokenStream was just created, the current token is initialized
// with random data (i.e. not initialized). It is therefore important that
// one of the first four member functions listed below is called first.
// The circular buffer lets us go back up to two tokens from the last
// scanned token. Internally, the relative number of backward steps that were
// taken (via ungetToken()) after the last token was scanned is stored in
// |lookahead|.
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
class MOZ_STACK_CLASS TokenStream
{
    // Unicode separators that are treated as line terminators, in addition to \n, \r.
    enum {
        LINE_SEPARATOR = 0x2028,
        PARA_SEPARATOR = 0x2029
    };

    static const size_t ntokens = 4;                // 1 current + 2 lookahead, rounded
                                                    // to power of 2 to avoid divmod by 3
    static const unsigned maxLookahead = 2;
    static const unsigned ntokensMask = ntokens - 1;

  public:
    typedef Vector<char16_t, 32> CharBuffer;

    TokenStream(ExclusiveContext* cx, const ReadOnlyCompileOptions& options,
                const char16_t* base, size_t length, StrictModeGetter* smg);

    ~TokenStream();

    bool checkOptions();

    // Accessors.
    const Token& currentToken() const { return tokens[cursor]; }
    bool isCurrentTokenType(TokenKind type) const {
        return currentToken().type == type;
    }
    const CharBuffer& getTokenbuf() const { return tokenbuf; }
    const char* getFilename() const { return filename; }
    unsigned getLineno() const { return lineno; }
    unsigned getColumn() const { return userbuf.offset() - linebase - 1; }
    bool getMutedErrors() const { return mutedErrors; }
    JSVersion versionNumber() const { return VersionNumber(options().version); }
    JSVersion versionWithFlags() const { return options().version; }

    PropertyName* currentName() const {
        if (isCurrentTokenType(TOK_YIELD))
            return cx->names().yield;
        MOZ_ASSERT(isCurrentTokenType(TOK_NAME));
        return currentToken().name();
    }

    bool isCurrentTokenAssignment() const {
        return TokenKindIsAssignment(currentToken().type);
    }

    // Flag methods.
    bool isEOF() const { return flags.isEOF; }
    bool sawOctalEscape() const { return flags.sawOctalEscape; }
    bool hadError() const { return flags.hadError; }

    // TokenStream-specific error reporters.
    bool reportError(unsigned errorNumber, ...);
    bool reportErrorNoOffset(unsigned errorNumber, ...);
    bool reportWarning(unsigned errorNumber, ...);

    static const uint32_t NoOffset = UINT32_MAX;

    // General-purpose error reporters.  You should avoid calling these
    // directly, and instead use the more succinct alternatives (e.g.
    // reportError()) in TokenStream, Parser, and BytecodeEmitter.
    bool reportCompileErrorNumberVA(uint32_t offset, unsigned flags, unsigned errorNumber,
                                    va_list args);
    bool reportStrictModeErrorNumberVA(uint32_t offset, bool strictMode, unsigned errorNumber,
                                       va_list args);
    bool reportStrictWarningErrorNumberVA(uint32_t offset, unsigned errorNumber,
                                          va_list args);

    // asm.js reporter
    void reportAsmJSError(uint32_t offset, unsigned errorNumber, ...);

    JSAtom* getRawTemplateStringAtom() {
        MOZ_ASSERT(currentToken().type == TOK_TEMPLATE_HEAD ||
                   currentToken().type == TOK_NO_SUBS_TEMPLATE);
        const char16_t* cur = userbuf.rawCharPtrAt(currentToken().pos.begin + 1);
        const char16_t* end;
        if (currentToken().type == TOK_TEMPLATE_HEAD) {
            // Of the form    |`...${|   or   |}...${|
            end = userbuf.rawCharPtrAt(currentToken().pos.end - 2);
        } else {
            // NO_SUBS_TEMPLATE is of the form   |`...`|   or   |}...`|
            end = userbuf.rawCharPtrAt(currentToken().pos.end - 1);
        }

        CharBuffer charbuf(cx);
        while (cur < end) {
            int32_t ch = *cur;
            if (ch == '\r') {
                ch = '\n';
                if ((cur + 1 < end) && (*(cur + 1) == '\n'))
                    cur++;
            }
            if (!charbuf.append(ch))
                return nullptr;
            cur++;
        }
        return AtomizeChars(cx, charbuf.begin(), charbuf.length());
    }

  private:
    // These are private because they should only be called by the tokenizer
    // while tokenizing not by, for example, BytecodeEmitter.
    bool reportStrictModeError(unsigned errorNumber, ...);
    bool strictMode() const { return strictModeGetter && strictModeGetter->strictMode(); }

    static JSAtom* atomize(ExclusiveContext* cx, CharBuffer& cb);
    bool putIdentInTokenbuf(const char16_t* identStart);

    struct Flags
    {
        bool isEOF:1;           // Hit end of file.
        bool isDirtyLine:1;     // Non-whitespace since start of line.
        bool sawOctalEscape:1;  // Saw an octal character escape.
        bool hadError:1;        // Hit a syntax error, at start or during a
                                // token.

        Flags()
          : isEOF(), isDirtyLine(), sawOctalEscape(), hadError()
        {}
    };

  public:
    // Sometimes the parser needs to modify how tokens are created.
    enum Modifier
    {
        None,           // Normal operation.
        Operand,        // Looking for an operand, not an operator.  In
                        //   practice, this means that when '/' is seen,
                        //   we look for a regexp instead of just returning
                        //   TOK_DIV.
        KeywordIsName,  // Treat keywords as names by returning TOK_NAME.
        TemplateTail,   // Treat next characters as part of a template string
    };

    // Advance to the next token.  If the token stream encountered an error,
    // return false.  Otherwise return true and store the token kind in |*ttp|.
    bool getToken(TokenKind* ttp, Modifier modifier = None) {
        // Check for a pushed-back token resulting from mismatching lookahead.
        if (lookahead != 0) {
            MOZ_ASSERT(!flags.hadError);
            lookahead--;
            cursor = (cursor + 1) & ntokensMask;
            TokenKind tt = currentToken().type;
            MOZ_ASSERT(tt != TOK_EOL);
            *ttp = tt;
            return true;
        }

        return getTokenInternal(ttp, modifier);
    }

    // Push the last scanned token back into the stream.
    void ungetToken() {
        MOZ_ASSERT(lookahead < maxLookahead);
        lookahead++;
        cursor = (cursor - 1) & ntokensMask;
    }

    bool peekToken(TokenKind* ttp, Modifier modifier = None) {
        if (lookahead > 0) {
            MOZ_ASSERT(!flags.hadError);
            *ttp = tokens[(cursor + 1) & ntokensMask].type;
            return true;
        }
        if (!getTokenInternal(ttp, modifier))
            return false;
        ungetToken();
        return true;
    }

    bool peekTokenPos(TokenPos* posp, Modifier modifier = None) {
        if (lookahead == 0) {
            TokenKind tt;
            if (!getTokenInternal(&tt, modifier))
                return false;
            ungetToken();
            MOZ_ASSERT(lookahead != 0);
        } else {
            MOZ_ASSERT(!flags.hadError);
        }
        *posp = tokens[(cursor + 1) & ntokensMask].pos;
        return true;
    }

    // This is like peekToken(), with one exception:  if there is an EOL
    // between the end of the current token and the start of the next token, it
    // return true and store TOK_EOL in |*ttp|.  In that case, no token with
    // TOK_EOL is actually created, just a TOK_EOL TokenKind is returned, and
    // currentToken() shouldn't be consulted.  (This is the only place TOK_EOL
    // is produced.)
    MOZ_ALWAYS_INLINE bool
    peekTokenSameLine(TokenKind* ttp, Modifier modifier = None) {
        const Token& curr = currentToken();

        // If lookahead != 0, we have scanned ahead at least one token, and
        // |lineno| is the line that the furthest-scanned token ends on.  If
        // it's the same as the line that the current token ends on, that's a
        // stronger condition than what we are looking for, and we don't need
        // to return TOK_EOL.
        if (lookahead != 0 && srcCoords.isOnThisLine(curr.pos.end, lineno)) {
            MOZ_ASSERT(!flags.hadError);
            *ttp = tokens[(cursor + 1) & ntokensMask].type;
            return true;
        }

        // The above check misses two cases where we don't have to return
        // TOK_EOL.
        // - The next token starts on the same line, but is a multi-line token.
        // - The next token starts on the same line, but lookahead==2 and there
        //   is a newline between the next token and the one after that.
        // The following test is somewhat expensive but gets these cases (and
        // all others) right.
        TokenKind tmp;
        if (!getToken(&tmp, modifier))
            return false;
        const Token& next = currentToken();
        ungetToken();

        *ttp = srcCoords.lineNum(curr.pos.end) == srcCoords.lineNum(next.pos.begin)
             ? next.type
             : TOK_EOL;
        return true;
    }

    // Get the next token from the stream if its kind is |tt|.
    bool matchToken(bool* matchedp, TokenKind tt, Modifier modifier = None) {
        TokenKind token;
        if (!getToken(&token, modifier))
            return false;
        if (token == tt) {
            *matchedp = true;
        } else {
            ungetToken();
            *matchedp = false;
        }
        return true;
    }

    void consumeKnownToken(TokenKind tt) {
        bool matched;
        MOZ_ASSERT(lookahead != 0);
        MOZ_ALWAYS_TRUE(matchToken(&matched, tt));
        MOZ_ALWAYS_TRUE(matched);
    }

    bool matchContextualKeyword(bool* matchedp, Handle<PropertyName*> keyword) {
        TokenKind token;
        if (!getToken(&token))
            return false;
        if (token == TOK_NAME && currentToken().name() == keyword) {
            *matchedp = true;
        } else {
            *matchedp = false;
            ungetToken();
        }
        return true;
    }

    bool nextTokenEndsExpr(bool* endsExpr) {
        TokenKind tt;
        if (!peekToken(&tt))
            return false;
        *endsExpr = isExprEnding[tt];
        return true;
    }

    class MOZ_STACK_CLASS Position {
      public:
        // The Token fields may contain pointers to atoms, so for correct
        // rooting we must ensure collection of atoms is disabled while objects
        // of this class are live.  Do this by requiring a dummy AutoKeepAtoms
        // reference in the constructor.
        //
        // This class is explicity ignored by the analysis, so don't add any
        // more pointers to GC things here!
        explicit Position(AutoKeepAtoms&) { }
      private:
        Position(const Position&) = delete;
        friend class TokenStream;
        const char16_t* buf;
        Flags flags;
        unsigned lineno;
        size_t linebase;
        size_t prevLinebase;
        Token currentToken;
        unsigned lookahead;
        Token lookaheadTokens[maxLookahead];
    };

    void advance(size_t position);
    void tell(Position*);
    void seek(const Position& pos);
    bool seek(const Position& pos, const TokenStream& other);

    const char16_t* rawCharPtrAt(size_t offset) const {
        return userbuf.rawCharPtrAt(offset);
    }

    const char16_t* rawLimit() const {
        return userbuf.limit();
    }

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

    // If the name at s[0:length] is not a keyword in this version, return
    // true with *ttp unchanged.
    //
    // If it is a reserved word in this version and strictness mode, and thus
    // can't be present in correct code, report a SyntaxError and return false.
    //
    // If it is a keyword, like "if", the behavior depends on ttp. If ttp is
    // null, report a SyntaxError ("if is a reserved identifier") and return
    // false. If ttp is non-null, return true with the keyword's TokenKind in
    // *ttp.
    bool checkForKeyword(const KeywordInfo* kw, TokenKind* ttp);
    bool checkForKeyword(const char16_t* s, size_t length, TokenKind* ttp);
    bool checkForKeyword(JSAtom* atom, TokenKind* ttp);

    // This class maps a userbuf offset (which is 0-indexed) to a line number
    // (which is 1-indexed) and a column index (which is 0-indexed).
    class SourceCoords
    {
        // For a given buffer holding source code, |lineStartOffsets_| has one
        // element per line of source code, plus one sentinel element.  Each
        // non-sentinel element holds the buffer offset for the start of the
        // corresponding line of source code.  For this example script:
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
        // The first element is always 0, and the last element is always the
        // MAX_PTR sentinel.
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

        // This is mutable because it's modified on every search, but that fact
        // isn't visible outside this class.
        mutable uint32_t    lastLineIndex_;

        uint32_t lineIndexOf(uint32_t offset) const;

        static const uint32_t MAX_PTR = UINT32_MAX;

        uint32_t lineIndexToNum(uint32_t lineIndex) const { return lineIndex + initialLineNum_; }
        uint32_t lineNumToIndex(uint32_t lineNum)   const { return lineNum   - initialLineNum_; }

      public:
        SourceCoords(ExclusiveContext* cx, uint32_t ln);

        void add(uint32_t lineNum, uint32_t lineStartOffset);
        bool fill(const SourceCoords& other);

        bool isOnThisLine(uint32_t offset, uint32_t lineNum) const {
            uint32_t lineIndex = lineNumToIndex(lineNum);
            MOZ_ASSERT(lineIndex + 1 < lineStartOffsets_.length());  // +1 due to sentinel
            return lineStartOffsets_[lineIndex] <= offset &&
                   offset < lineStartOffsets_[lineIndex + 1];
        }

        uint32_t lineNum(uint32_t offset) const;
        uint32_t columnIndex(uint32_t offset) const;
        void lineNumAndColumnIndex(uint32_t offset, uint32_t* lineNum, uint32_t* columnIndex) const;
    };

    SourceCoords srcCoords;

    JSAtomState& names() const {
        return cx->names();
    }

    ExclusiveContext* context() const {
        return cx;
    }

    const ReadOnlyCompileOptions& options() const {
        return options_;
    }

  private:
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
    class TokenBuf {
      public:
        TokenBuf(ExclusiveContext* cx, const char16_t* buf, size_t length, size_t startOffset)
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

        const char16_t* rawCharPtrAt(size_t offset) const {
            MOZ_ASSERT(startOffset_ <= offset);
            MOZ_ASSERT(offset - startOffset_ <= mozilla::PointerRangeSize(base_, limit_));
            return base_ + (offset - startOffset_);
        }

        const char16_t* limit() const {
            return limit_;
        }

        char16_t getRawChar() {
            return *ptr++;      // this will nullptr-crash if poisoned
        }

        char16_t peekRawChar() const {
            return *ptr;        // this will nullptr-crash if poisoned
        }

        bool matchRawChar(char16_t c) {
            if (*ptr == c) {    // this will nullptr-crash if poisoned
                ptr++;
                return true;
            }
            return false;
        }

        bool matchRawCharBackwards(char16_t c) {
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

        const char16_t* addressOfNextRawChar(bool allowPoisoned = false) const {
            MOZ_ASSERT_IF(!allowPoisoned, ptr);     // make sure it hasn't been poisoned
            return ptr;
        }

        // Use this with caution!
        void setAddressOfNextRawChar(const char16_t* a, bool allowPoisoned = false) {
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
            return c == '\n' || c == '\r' || c == LINE_SEPARATOR || c == PARA_SEPARATOR;
        }

        // Returns the offset of the next EOL, but stops once 'max' characters
        // have been scanned (*including* the char at startOffset_).
        size_t findEOLMax(size_t start, size_t max);

      private:
        const char16_t* base_;          // base of buffer
        uint32_t startOffset_;          // offset of base_[0]
        const char16_t* limit_;         // limit for quick bounds check
        const char16_t* ptr;            // next char to get
    };

    bool getTokenInternal(TokenKind* ttp, Modifier modifier);

    bool getStringOrTemplateToken(int untilChar, Token** tp);

    int32_t getChar();
    int32_t getCharIgnoreEOL();
    void ungetChar(int32_t c);
    void ungetCharIgnoreEOL(int32_t c);
    Token* newToken(ptrdiff_t adjust);
    bool peekUnicodeEscape(int32_t* c);
    bool matchUnicodeEscapeIdStart(int32_t* c);
    bool matchUnicodeEscapeIdent(int32_t* c);
    bool peekChars(int n, char16_t* cp);

    bool getDirectives(bool isMultiline, bool shouldWarnDeprecated);
    bool getDirective(bool isMultiline, bool shouldWarnDeprecated,
                      const char* directive, int directiveLength,
                      const char* errorMsgPragma,
                      mozilla::UniquePtr<char16_t[], JS::FreePolicy>* destination);
    bool getDisplayURL(bool isMultiline, bool shouldWarnDeprecated);
    bool getSourceMappingURL(bool isMultiline, bool shouldWarnDeprecated);

    // |expect| cannot be an EOL char.
    bool matchChar(int32_t expect) {
        MOZ_ASSERT(!TokenBuf::isRawEOLChar(expect));
        return MOZ_LIKELY(userbuf.hasRawChars()) &&
               userbuf.matchRawChar(expect);
    }

    void consumeKnownChar(int32_t expect) {
        mozilla::DebugOnly<int32_t> c = getChar();
        MOZ_ASSERT(c == expect);
    }

    int32_t peekChar() {
        int32_t c = getChar();
        ungetChar(c);
        return c;
    }

    void skipChars(int n) {
        while (--n >= 0)
            getChar();
    }

    void updateLineInfoForEOL();
    void updateFlagsForEOL();

    // Options used for parsing/tokenizing.
    const ReadOnlyCompileOptions& options_;

    Token               tokens[ntokens];    // circular token buffer
    unsigned            cursor;             // index of last parsed token
    unsigned            lookahead;          // count of lookahead tokens
    unsigned            lineno;             // current line number
    Flags               flags;              // flags -- see above
    size_t              linebase;           // start of current line
    size_t              prevLinebase;       // start of previous line;  size_t(-1) if on the first line
    TokenBuf            userbuf;            // user input buffer
    const char*         filename;          // input filename or null
    mozilla::UniquePtr<char16_t[], JS::FreePolicy> displayURL_; // the user's requested source URL or null
    mozilla::UniquePtr<char16_t[], JS::FreePolicy> sourceMapURL_; // source map's filename or null
    CharBuffer          tokenbuf;           // current token string buffer
    uint8_t             isExprEnding[TOK_LIMIT];// which tokens definitely terminate exprs?
    ExclusiveContext*   const cx;
    bool                mutedErrors;
    StrictModeGetter*   strictModeGetter;  // used to test for strict mode
};

// Steal one JSREPORT_* bit (see jsapi.h) to tell that arguments to the error
// message have const char16_t* type, not const char*.
#define JSREPORT_UC 0x100

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
