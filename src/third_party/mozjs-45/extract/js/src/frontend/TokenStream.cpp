/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// JS lexical scanner.

#include "frontend/TokenStream.h"

#include "mozilla/IntegerTypeTraits.h"
#include "mozilla/PodOperations.h"
#include "mozilla/UniquePtr.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jsatom.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsexn.h"
#include "jsnum.h"

#include "frontend/BytecodeCompiler.h"
#include "js/CharacterEncoding.h"
#include "vm/HelperThreads.h"
#include "vm/Keywords.h"
#include "vm/StringBuffer.h"

using namespace js;
using namespace js::frontend;
using namespace js::unicode;

using mozilla::Maybe;
using mozilla::PodAssign;
using mozilla::PodCopy;
using mozilla::PodZero;
using mozilla::UniquePtr;

struct KeywordInfo {
    const char* chars;         // C string with keyword text
    TokenKind   tokentype;
};

static const KeywordInfo keywords[] = {
#define KEYWORD_INFO(keyword, name, type) \
    {js_##keyword##_str, type},
    FOR_EACH_JAVASCRIPT_KEYWORD(KEYWORD_INFO)
#undef KEYWORD_INFO
};

// Returns a KeywordInfo for the specified characters, or nullptr if the string
// is not a keyword.
template <typename CharT>
static const KeywordInfo*
FindKeyword(const CharT* s, size_t length)
{
    MOZ_ASSERT(length != 0);

    size_t i;
    const KeywordInfo* kw;
    const char* chars;

#define JSKW_LENGTH()           length
#define JSKW_AT(column)         s[column]
#define JSKW_GOT_MATCH(index)   i = (index); goto got_match;
#define JSKW_TEST_GUESS(index)  i = (index); goto test_guess;
#define JSKW_NO_MATCH()         goto no_match;
#include "jsautokw.h"
#undef JSKW_NO_MATCH
#undef JSKW_TEST_GUESS
#undef JSKW_GOT_MATCH
#undef JSKW_AT
#undef JSKW_LENGTH

  got_match:
    return &keywords[i];

  test_guess:
    kw = &keywords[i];
    chars = kw->chars;
    do {
        if (*s++ != (unsigned char)(*chars++))
            goto no_match;
    } while (--length != 0);
    return kw;

  no_match:
    return nullptr;
}

static const KeywordInfo*
FindKeyword(JSLinearString* str)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? FindKeyword(str->latin1Chars(nogc), str->length())
           : FindKeyword(str->twoByteChars(nogc), str->length());
}

template <typename CharT>
static bool
IsIdentifier(const CharT* chars, size_t length)
{
    if (length == 0)
        return false;

    if (!IsIdentifierStart(*chars))
        return false;

    const CharT* end = chars + length;
    while (++chars != end) {
        if (!IsIdentifierPart(*chars))
            return false;
    }

    return true;
}

bool
frontend::IsIdentifier(JSLinearString* str)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::IsIdentifier(str->latin1Chars(nogc), str->length())
           : ::IsIdentifier(str->twoByteChars(nogc), str->length());
}

bool
frontend::IsIdentifier(const char16_t* chars, size_t length)
{
    return ::IsIdentifier(chars, length);
}

bool
frontend::IsKeyword(JSLinearString* str)
{
    return FindKeyword(str) != nullptr;
}

TokenStream::SourceCoords::SourceCoords(ExclusiveContext* cx, uint32_t ln)
  : lineStartOffsets_(cx), initialLineNum_(ln), lastLineIndex_(0)
{
    // This is actually necessary!  Removing it causes compile errors on
    // GCC and clang.  You could try declaring this:
    //
    //   const uint32_t TokenStream::SourceCoords::MAX_PTR;
    //
    // which fixes the GCC/clang error, but causes bustage on Windows.  Sigh.
    //
    uint32_t maxPtr = MAX_PTR;

    // The first line begins at buffer offset 0.  MAX_PTR is the sentinel.  The
    // appends cannot fail because |lineStartOffsets_| has statically-allocated
    // elements.
    MOZ_ASSERT(lineStartOffsets_.capacity() >= 2);
    MOZ_ALWAYS_TRUE(lineStartOffsets_.reserve(2));
    lineStartOffsets_.infallibleAppend(0);
    lineStartOffsets_.infallibleAppend(maxPtr);
}

MOZ_ALWAYS_INLINE bool
TokenStream::SourceCoords::add(uint32_t lineNum, uint32_t lineStartOffset)
{
    uint32_t lineIndex = lineNumToIndex(lineNum);
    uint32_t sentinelIndex = lineStartOffsets_.length() - 1;

    MOZ_ASSERT(lineStartOffsets_[0] == 0 && lineStartOffsets_[sentinelIndex] == MAX_PTR);

    if (lineIndex == sentinelIndex) {
        // We haven't seen this newline before.  Update lineStartOffsets_
        // only if lineStartOffsets_.append succeeds, to keep sentinel.
        // Otherwise return false to tell TokenStream about OOM.
        uint32_t maxPtr = MAX_PTR;
        if (!lineStartOffsets_.append(maxPtr))
            return false;

        lineStartOffsets_[lineIndex] = lineStartOffset;
    } else {
        // We have seen this newline before (and ungot it).  Do nothing (other
        // than checking it hasn't mysteriously changed).
        // This path can be executed after hitting OOM, so check lineIndex.
        MOZ_ASSERT_IF(lineIndex < sentinelIndex, lineStartOffsets_[lineIndex] == lineStartOffset);
    }
    return true;
}

MOZ_ALWAYS_INLINE bool
TokenStream::SourceCoords::fill(const TokenStream::SourceCoords& other)
{
    MOZ_ASSERT(lineStartOffsets_.back() == MAX_PTR);
    MOZ_ASSERT(other.lineStartOffsets_.back() == MAX_PTR);

    if (lineStartOffsets_.length() >= other.lineStartOffsets_.length())
        return true;

    uint32_t sentinelIndex = lineStartOffsets_.length() - 1;
    lineStartOffsets_[sentinelIndex] = other.lineStartOffsets_[sentinelIndex];

    for (size_t i = sentinelIndex + 1; i < other.lineStartOffsets_.length(); i++) {
        if (!lineStartOffsets_.append(other.lineStartOffsets_[i]))
            return false;
    }
    return true;
}

MOZ_ALWAYS_INLINE uint32_t
TokenStream::SourceCoords::lineIndexOf(uint32_t offset) const
{
    uint32_t iMin, iMax, iMid;

    if (lineStartOffsets_[lastLineIndex_] <= offset) {
        // If we reach here, offset is on a line the same as or higher than
        // last time.  Check first for the +0, +1, +2 cases, because they
        // typically cover 85--98% of cases.
        if (offset < lineStartOffsets_[lastLineIndex_ + 1])
            return lastLineIndex_;      // lineIndex is same as last time

        // If we reach here, there must be at least one more entry (plus the
        // sentinel).  Try it.
        lastLineIndex_++;
        if (offset < lineStartOffsets_[lastLineIndex_ + 1])
            return lastLineIndex_;      // lineIndex is one higher than last time

        // The same logic applies here.
        lastLineIndex_++;
        if (offset < lineStartOffsets_[lastLineIndex_ + 1]) {
            return lastLineIndex_;      // lineIndex is two higher than last time
        }

        // No luck.  Oh well, we have a better-than-default starting point for
        // the binary search.
        iMin = lastLineIndex_ + 1;
        MOZ_ASSERT(iMin < lineStartOffsets_.length() - 1);   // -1 due to the sentinel

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
        if (offset >= lineStartOffsets_[iMid + 1])
            iMin = iMid + 1;    // offset is above lineStartOffsets_[iMid]
        else
            iMax = iMid;        // offset is below or within lineStartOffsets_[iMid]
    }
    MOZ_ASSERT(iMax == iMin);
    MOZ_ASSERT(lineStartOffsets_[iMin] <= offset && offset < lineStartOffsets_[iMin + 1]);
    lastLineIndex_ = iMin;
    return iMin;
}

uint32_t
TokenStream::SourceCoords::lineNum(uint32_t offset) const
{
    uint32_t lineIndex = lineIndexOf(offset);
    return lineIndexToNum(lineIndex);
}

uint32_t
TokenStream::SourceCoords::columnIndex(uint32_t offset) const
{
    uint32_t lineIndex = lineIndexOf(offset);
    uint32_t lineStartOffset = lineStartOffsets_[lineIndex];
    MOZ_ASSERT(offset >= lineStartOffset);
    return offset - lineStartOffset;
}

void
TokenStream::SourceCoords::lineNumAndColumnIndex(uint32_t offset, uint32_t* lineNum,
                                                 uint32_t* columnIndex) const
{
    uint32_t lineIndex = lineIndexOf(offset);
    *lineNum = lineIndexToNum(lineIndex);
    uint32_t lineStartOffset = lineStartOffsets_[lineIndex];
    MOZ_ASSERT(offset >= lineStartOffset);
    *columnIndex = offset - lineStartOffset;
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4351)
#endif

TokenStream::TokenStream(ExclusiveContext* cx, const ReadOnlyCompileOptions& options,
                         const char16_t* base, size_t length, StrictModeGetter* smg)
  : srcCoords(cx, options.lineno),
    options_(options),
    tokens(),
    cursor(),
    lookahead(),
    lineno(options.lineno),
    flags(),
    linebase(0),
    prevLinebase(size_t(-1)),
    userbuf(cx, base, length, options.column),
    filename(options.filename()),
    displayURL_(nullptr),
    sourceMapURL_(nullptr),
    tokenbuf(cx),
    cx(cx),
    mutedErrors(options.mutedErrors()),
    strictModeGetter(smg)
{
    // Nb: the following tables could be static, but initializing them here is
    // much easier.  Don't worry, the time to initialize them for each
    // TokenStream is trivial.  See bug 639420.

    // See Parser::assignExpr() for an explanation of isExprEnding[].
    memset(isExprEnding, 0, sizeof(isExprEnding));
    isExprEnding[TOK_COMMA] = 1;
    isExprEnding[TOK_SEMI]  = 1;
    isExprEnding[TOK_COLON] = 1;
    isExprEnding[TOK_RP]    = 1;
    isExprEnding[TOK_RB]    = 1;
    isExprEnding[TOK_RC]    = 1;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

bool
TokenStream::checkOptions()
{
    // Constrain starting columns to half of the range of a signed 32-bit value,
    // to avoid overflow.
    if (options().column >= mozilla::MaxValue<int32_t>::value / 2 + 1) {
        reportErrorNoOffset(JSMSG_BAD_COLUMN_NUMBER);
        return false;
    }

    return true;
}

TokenStream::~TokenStream()
{
}

// Use the fastest available getc.
#if defined(HAVE_GETC_UNLOCKED)
# define fast_getc getc_unlocked
#elif defined(HAVE__GETC_NOLOCK)
# define fast_getc _getc_nolock
#else
# define fast_getc getc
#endif

MOZ_ALWAYS_INLINE void
TokenStream::updateLineInfoForEOL()
{
    prevLinebase = linebase;
    linebase = userbuf.offset();
    lineno++;
    if (!srcCoords.add(lineno, linebase))
        flags.hitOOM = true;
}

MOZ_ALWAYS_INLINE void
TokenStream::updateFlagsForEOL()
{
    flags.isDirtyLine = false;
}

// This gets the next char, normalizing all EOL sequences to '\n' as it goes.
int32_t
TokenStream::getChar()
{
    int32_t c;
    if (MOZ_LIKELY(userbuf.hasRawChars())) {
        c = userbuf.getRawChar();

        // Normalize the char16_t if it was a newline.
        if (MOZ_UNLIKELY(c == '\n'))
            goto eol;
        if (MOZ_UNLIKELY(c == '\r')) {
            // If it's a \r\n sequence: treat as a single EOL, skip over the \n.
            if (MOZ_LIKELY(userbuf.hasRawChars()))
                userbuf.matchRawChar('\n');
            goto eol;
        }
        if (MOZ_UNLIKELY(c == LINE_SEPARATOR || c == PARA_SEPARATOR))
            goto eol;

        return c;
    }

    flags.isEOF = true;
    return EOF;

  eol:
    updateLineInfoForEOL();
    return '\n';
}

// This gets the next char. It does nothing special with EOL sequences, not
// even updating the line counters.  It can be used safely if (a) the
// resulting char is guaranteed to be ungotten (by ungetCharIgnoreEOL()) if
// it's an EOL, and (b) the line-related state (lineno, linebase) is not used
// before it's ungotten.
int32_t
TokenStream::getCharIgnoreEOL()
{
    if (MOZ_LIKELY(userbuf.hasRawChars()))
        return userbuf.getRawChar();

    flags.isEOF = true;
    return EOF;
}

void
TokenStream::ungetChar(int32_t c)
{
    if (c == EOF)
        return;
    MOZ_ASSERT(!userbuf.atStart());
    userbuf.ungetRawChar();
    if (c == '\n') {
#ifdef DEBUG
        int32_t c2 = userbuf.peekRawChar();
        MOZ_ASSERT(TokenBuf::isRawEOLChar(c2));
#endif

        // If it's a \r\n sequence, also unget the \r.
        if (!userbuf.atStart())
            userbuf.matchRawCharBackwards('\r');

        MOZ_ASSERT(prevLinebase != size_t(-1));    // we should never get more than one EOL char
        linebase = prevLinebase;
        prevLinebase = size_t(-1);
        lineno--;
    } else {
        MOZ_ASSERT(userbuf.peekRawChar() == c);
    }
}

void
TokenStream::ungetCharIgnoreEOL(int32_t c)
{
    if (c == EOF)
        return;
    MOZ_ASSERT(!userbuf.atStart());
    userbuf.ungetRawChar();
}

// Return true iff |n| raw characters can be read from this without reading past
// EOF or a newline, and copy those characters into |cp| if so.  The characters
// are not consumed: use skipChars(n) to do so after checking that the consumed
// characters had appropriate values.
bool
TokenStream::peekChars(int n, char16_t* cp)
{
    int i, j;
    int32_t c;

    for (i = 0; i < n; i++) {
        c = getCharIgnoreEOL();
        if (c == EOF)
            break;
        if (c == '\n') {
            ungetCharIgnoreEOL(c);
            break;
        }
        cp[i] = char16_t(c);
    }
    for (j = i - 1; j >= 0; j--)
        ungetCharIgnoreEOL(cp[j]);
    return i == n;
}

size_t
TokenStream::TokenBuf::findEOLMax(size_t start, size_t max)
{
    const char16_t* p = rawCharPtrAt(start);

    size_t n = 0;
    while (true) {
        if (p >= limit_)
            break;
        if (n >= max)
            break;
        n++;
        if (TokenBuf::isRawEOLChar(*p++))
            break;
    }
    return start + n;
}

bool
TokenStream::advance(size_t position)
{
    const char16_t* end = userbuf.rawCharPtrAt(position);
    while (userbuf.addressOfNextRawChar() < end)
        getChar();

    Token* cur = &tokens[cursor];
    cur->pos.begin = userbuf.offset();
    MOZ_MAKE_MEM_UNDEFINED(&cur->type, sizeof(cur->type));
    lookahead = 0;

    if (flags.hitOOM)
        return reportError(JSMSG_OUT_OF_MEMORY);

    return true;
}

void
TokenStream::tell(Position* pos)
{
    pos->buf = userbuf.addressOfNextRawChar(/* allowPoisoned = */ true);
    pos->flags = flags;
    pos->lineno = lineno;
    pos->linebase = linebase;
    pos->prevLinebase = prevLinebase;
    pos->lookahead = lookahead;
    pos->currentToken = currentToken();
    for (unsigned i = 0; i < lookahead; i++)
        pos->lookaheadTokens[i] = tokens[(cursor + 1 + i) & ntokensMask];
}

void
TokenStream::seek(const Position& pos)
{
    userbuf.setAddressOfNextRawChar(pos.buf, /* allowPoisoned = */ true);
    flags = pos.flags;
    lineno = pos.lineno;
    linebase = pos.linebase;
    prevLinebase = pos.prevLinebase;
    lookahead = pos.lookahead;

    tokens[cursor] = pos.currentToken;
    for (unsigned i = 0; i < lookahead; i++)
        tokens[(cursor + 1 + i) & ntokensMask] = pos.lookaheadTokens[i];
}

bool
TokenStream::seek(const Position& pos, const TokenStream& other)
{
    if (!srcCoords.fill(other.srcCoords))
        return false;
    seek(pos);
    return true;
}

bool
TokenStream::reportStrictModeErrorNumberVA(uint32_t offset, bool strictMode, unsigned errorNumber,
                                           va_list args)
{
    // In strict mode code, this is an error, not merely a warning.
    unsigned flags;
    if (strictMode)
        flags = JSREPORT_ERROR;
    else if (options().extraWarningsOption)
        flags = JSREPORT_WARNING | JSREPORT_STRICT;
    else
        return true;

    return reportCompileErrorNumberVA(offset, flags, errorNumber, args);
}

void
CompileError::throwError(JSContext* cx)
{
    // If there's a runtime exception type associated with this error
    // number, set that as the pending exception.  For errors occuring at
    // compile time, this is very likely to be a JSEXN_SYNTAXERR.
    //
    // If an exception is thrown but not caught, the JSREPORT_EXCEPTION
    // flag will be set in report.flags.  Proper behavior for an error
    // reporter is to ignore a report with this flag for all but top-level
    // compilation errors.  The exception will remain pending, and so long
    // as the non-top-level "load", "eval", or "compile" native function
    // returns false, the top-level reporter will eventually receive the
    // uncaught exception report.
    if (!ErrorToException(cx, message, &report, nullptr, nullptr))
        CallErrorReporter(cx, message, &report);
}

CompileError::~CompileError()
{
    js_free((void*)report.uclinebuf);
    js_free((void*)report.linebuf);
    js_free((void*)report.ucmessage);
    js_free(message);
    message = nullptr;

    if (report.messageArgs) {
        if (argumentsType == ArgumentsAreASCII) {
            unsigned i = 0;
            while (report.messageArgs[i])
                js_free((void*)report.messageArgs[i++]);
        }
        js_free(report.messageArgs);
    }

    PodZero(&report);
}

bool
TokenStream::reportCompileErrorNumberVA(uint32_t offset, unsigned flags, unsigned errorNumber,
                                        va_list args)
{
    bool warning = JSREPORT_IS_WARNING(flags);

    if (warning && options().werrorOption) {
        flags &= ~JSREPORT_WARNING;
        warning = false;
    }

    // On the main thread, report the error immediately. When compiling off
    // thread, save the error so that the main thread can report it later.
    CompileError tempErr;
    CompileError& err = cx->isJSContext() ? tempErr : cx->addPendingCompileError();

    err.report.flags = flags;
    err.report.errorNumber = errorNumber;
    err.report.filename = filename;
    err.report.isMuted = mutedErrors;
    if (offset == NoOffset) {
        err.report.lineno = 0;
        err.report.column = 0;
    } else {
        err.report.lineno = srcCoords.lineNum(offset);
        err.report.column = srcCoords.columnIndex(offset);
    }

    // If we have no location information, try to get one from the caller.
    bool callerFilename = false;
    if (offset != NoOffset && !err.report.filename && cx->isJSContext()) {
        NonBuiltinFrameIter iter(cx->asJSContext(),
                                 FrameIter::ALL_CONTEXTS, FrameIter::GO_THROUGH_SAVED,
                                 FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK,
                                 cx->compartment()->principals());
        if (!iter.done() && iter.scriptFilename()) {
            callerFilename = true;
            err.report.filename = iter.scriptFilename();
            err.report.lineno = iter.computeLine(&err.report.column);
        }
    }

    err.argumentsType = (flags & JSREPORT_UC) ? ArgumentsAreUnicode : ArgumentsAreASCII;

    if (!ExpandErrorArgumentsVA(cx, GetErrorMessage, nullptr, errorNumber, &err.message,
                                &err.report, err.argumentsType, args))
    {
        return false;
    }

    // Given a token, T, that we want to complain about: if T's (starting)
    // lineno doesn't match TokenStream's lineno, that means we've scanned past
    // the line that T starts on, which makes it hard to print some or all of
    // T's (starting) line for context.
    //
    // So we don't even try, leaving report.linebuf and friends zeroed.  This
    // means that any error involving a multi-line token (e.g. an unterminated
    // multi-line string literal) won't have a context printed.
    if (offset != NoOffset && err.report.lineno == lineno && !callerFilename) {
        // We show only a portion (a "window") of the line around the erroneous
        // token -- the first char in the token, plus |windowRadius| chars
        // before it and |windowRadius - 1| chars after it.  This is because
        // lines can be very long and printing the whole line is (a) not that
        // helpful, and (b) can waste a lot of memory.  See bug 634444.
        static const size_t windowRadius = 60;

        // The window must start within the current line, no earlier than
        // windowRadius characters before offset.
        size_t windowStart = (offset - linebase > windowRadius) ?
                             offset - windowRadius :
                             linebase;

        // The window must start within the portion of the current line
        // that we actually have in our buffer.
        if (windowStart < userbuf.startOffset())
            windowStart = userbuf.startOffset();

        // The window must end within the current line, no later than
        // windowRadius after offset.
        size_t windowEnd = userbuf.findEOLMax(offset, windowRadius);
        size_t windowLength = windowEnd - windowStart;
        MOZ_ASSERT(windowLength <= windowRadius * 2);

        // Create the windowed strings.
        StringBuffer windowBuf(cx);
        if (!windowBuf.append(userbuf.rawCharPtrAt(windowStart), windowLength) ||
            !windowBuf.append((char16_t)0))
            return false;

        // Unicode and char versions of the window into the offending source
        // line, without final \n.
        err.report.uclinebuf = windowBuf.stealChars();
        if (!err.report.uclinebuf)
            return false;

        mozilla::Range<const char16_t> tbchars(err.report.uclinebuf, windowLength);
        err.report.linebuf = JS::LossyTwoByteCharsToNewLatin1CharsZ(cx, tbchars).c_str();
        if (!err.report.linebuf)
            return false;

        err.report.tokenptr = err.report.linebuf + (offset - windowStart);
        err.report.uctokenptr = err.report.uclinebuf + (offset - windowStart);
    }

    if (cx->isJSContext())
        err.throwError(cx->asJSContext());

    return warning;
}

bool
TokenStream::reportStrictModeError(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);
    bool result = reportStrictModeErrorNumberVA(currentToken().pos.begin, strictMode(),
                                                errorNumber, args);
    va_end(args);
    return result;
}

bool
TokenStream::reportError(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);
    bool result = reportCompileErrorNumberVA(currentToken().pos.begin, JSREPORT_ERROR, errorNumber,
                                             args);
    va_end(args);
    return result;
}

bool
TokenStream::reportErrorNoOffset(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);
    bool result = reportCompileErrorNumberVA(NoOffset, JSREPORT_ERROR, errorNumber,
                                             args);
    va_end(args);
    return result;
}

bool
TokenStream::reportWarning(unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);
    bool result = reportCompileErrorNumberVA(currentToken().pos.begin, JSREPORT_WARNING,
                                             errorNumber, args);
    va_end(args);
    return result;
}

bool
TokenStream::reportStrictWarningErrorNumberVA(uint32_t offset, unsigned errorNumber, va_list args)
{
    if (!options().extraWarningsOption)
        return true;

    return reportCompileErrorNumberVA(offset, JSREPORT_STRICT|JSREPORT_WARNING, errorNumber, args);
}

void
TokenStream::reportAsmJSError(uint32_t offset, unsigned errorNumber, ...)
{
    va_list args;
    va_start(args, errorNumber);
    unsigned flags = options().throwOnAsmJSValidationFailureOption
                     ? JSREPORT_ERROR
                     : JSREPORT_WARNING;
    reportCompileErrorNumberVA(offset, flags, errorNumber, args);
    va_end(args);
}

// We have encountered a '\': check for a Unicode escape sequence after it.
// Return 'true' and the character code value (by value) if we found a
// Unicode escape sequence.  Otherwise, return 'false'.  In both cases, do not
// advance along the buffer.
bool
TokenStream::peekUnicodeEscape(int* result)
{
    char16_t cp[5];

    if (peekChars(5, cp) && cp[0] == 'u' &&
        JS7_ISHEX(cp[1]) && JS7_ISHEX(cp[2]) &&
        JS7_ISHEX(cp[3]) && JS7_ISHEX(cp[4]))
    {
        *result = (((((JS7_UNHEX(cp[1]) << 4)
                + JS7_UNHEX(cp[2])) << 4)
              + JS7_UNHEX(cp[3])) << 4)
            + JS7_UNHEX(cp[4]);
        return true;
    }
    return false;
}

bool
TokenStream::matchUnicodeEscapeIdStart(int32_t* cp)
{
    if (peekUnicodeEscape(cp) && IsIdentifierStart(*cp)) {
        skipChars(5);
        return true;
    }
    return false;
}

bool
TokenStream::matchUnicodeEscapeIdent(int32_t* cp)
{
    if (peekUnicodeEscape(cp) && IsIdentifierPart(*cp)) {
        skipChars(5);
        return true;
    }
    return false;
}

// Helper function which returns true if the first length(q) characters in p are
// the same as the characters in q.
static bool
CharsMatch(const char16_t* p, const char* q) {
    while (*q) {
        if (*p++ != *q++)
            return false;
    }
    return true;
}

bool
TokenStream::getDirectives(bool isMultiline, bool shouldWarnDeprecated)
{
    // Match directive comments used in debugging, such as "//# sourceURL" and
    // "//# sourceMappingURL". Use of "//@" instead of "//#" is deprecated.
    //
    // To avoid a crashing bug in IE, several JavaScript transpilers wrap single
    // line comments containing a source mapping URL inside a multiline
    // comment. To avoid potentially expensive lookahead and backtracking, we
    // only check for this case if we encounter a '#' character.

    if (!getDisplayURL(isMultiline, shouldWarnDeprecated))
        return false;
    if (!getSourceMappingURL(isMultiline, shouldWarnDeprecated))
        return false;

    return true;
}

bool
TokenStream::getDirective(bool isMultiline, bool shouldWarnDeprecated,
                          const char* directive, int directiveLength,
                          const char* errorMsgPragma,
                          UniquePtr<char16_t[], JS::FreePolicy>* destination)
{
    MOZ_ASSERT(directiveLength <= 18);
    char16_t peeked[18];
    int32_t c;

    if (peekChars(directiveLength, peeked) && CharsMatch(peeked, directive)) {
        if (shouldWarnDeprecated &&
            !reportWarning(JSMSG_DEPRECATED_PRAGMA, errorMsgPragma))
            return false;

        skipChars(directiveLength);
        tokenbuf.clear();

        while ((c = peekChar()) && c != EOF && !IsSpaceOrBOM2(c)) {
            getChar();
            // Debugging directives can occur in both single- and multi-line
            // comments. If we're currently inside a multi-line comment, we also
            // need to recognize multi-line comment terminators.
            if (isMultiline && c == '*' && peekChar() == '/') {
                ungetChar('*');
                break;
            }
            if (!tokenbuf.append(c))
                return false;
        }

        if (tokenbuf.empty()) {
            // The directive's URL was missing, but this is not quite an
            // exception that we should stop and drop everything for.
            return true;
        }

        size_t length = tokenbuf.length();

        *destination = cx->make_pod_array<char16_t>(length + 1);
        if (!*destination)
            return false;

        PodCopy(destination->get(), tokenbuf.begin(), length);
        (*destination)[length] = '\0';
    }

    return true;
}

bool
TokenStream::getDisplayURL(bool isMultiline, bool shouldWarnDeprecated)
{
    // Match comments of the form "//# sourceURL=<url>" or
    // "/\* //# sourceURL=<url> *\/"
    //
    // Note that while these are labeled "sourceURL" in the source text,
    // internally we refer to it as a "displayURL" to distinguish what the
    // developer would like to refer to the source as from the source's actual
    // URL.

    return getDirective(isMultiline, shouldWarnDeprecated, " sourceURL=", 11,
                        "sourceURL", &displayURL_);
}

bool
TokenStream::getSourceMappingURL(bool isMultiline, bool shouldWarnDeprecated)
{
    // Match comments of the form "//# sourceMappingURL=<url>" or
    // "/\* //# sourceMappingURL=<url> *\/"

    return getDirective(isMultiline, shouldWarnDeprecated, " sourceMappingURL=", 18,
                        "sourceMappingURL", &sourceMapURL_);
}

MOZ_ALWAYS_INLINE Token*
TokenStream::newToken(ptrdiff_t adjust)
{
    cursor = (cursor + 1) & ntokensMask;
    Token* tp = &tokens[cursor];
    tp->pos.begin = userbuf.offset() + adjust;

    // NOTE: tp->pos.end is not set until the very end of getTokenInternal().
    MOZ_MAKE_MEM_UNDEFINED(&tp->pos.end, sizeof(tp->pos.end));

    return tp;
}

MOZ_ALWAYS_INLINE JSAtom*
TokenStream::atomize(ExclusiveContext* cx, CharBuffer& cb)
{
    return AtomizeChars(cx, cb.begin(), cb.length());
}

#ifdef DEBUG
static bool
IsTokenSane(Token* tp)
{
    // Nb: TOK_EOL should never be used in an actual Token;  it should only be
    // returned as a TokenKind from peekTokenSameLine().
    if (tp->type < 0 || tp->type >= TOK_LIMIT || tp->type == TOK_EOL)
        return false;

    if (tp->pos.end < tp->pos.begin)
        return false;

    return true;
}
#endif

bool
TokenStream::putIdentInTokenbuf(const char16_t* identStart)
{
    int32_t c, qc;
    const char16_t* tmp = userbuf.addressOfNextRawChar();
    userbuf.setAddressOfNextRawChar(identStart);

    tokenbuf.clear();
    for (;;) {
        c = getCharIgnoreEOL();
        if (!IsIdentifierPart(c)) {
            if (c != '\\' || !matchUnicodeEscapeIdent(&qc))
                break;
            c = qc;
        }
        if (!tokenbuf.append(c)) {
            userbuf.setAddressOfNextRawChar(tmp);
            return false;
        }
    }
    userbuf.setAddressOfNextRawChar(tmp);
    return true;
}

bool
TokenStream::checkForKeyword(const KeywordInfo* kw, TokenKind* ttp)
{
    if (kw->tokentype == TOK_RESERVED)
        return reportError(JSMSG_RESERVED_ID, kw->chars);

    if (kw->tokentype == TOK_STRICT_RESERVED)
        return reportStrictModeError(JSMSG_RESERVED_ID, kw->chars);

    // Treat 'let' as an identifier and contextually a keyword in sloppy mode.
    // It is always a keyword in strict mode.
    if (kw->tokentype == TOK_LET && !strictMode())
        return true;

    // Working keyword.
    if (ttp) {
        *ttp = kw->tokentype;
        return true;
    }

    return reportError(JSMSG_RESERVED_ID, kw->chars);
}

bool
TokenStream::checkForKeyword(JSAtom* atom, TokenKind* ttp)
{
    const KeywordInfo* kw = FindKeyword(atom);
    if (!kw)
        return true;

    return checkForKeyword(kw, ttp);
}

enum FirstCharKind {
    // A char16_t has the 'OneChar' kind if it, by itself, constitutes a valid
    // token that cannot also be a prefix of a longer token.  E.g. ';' has the
    // OneChar kind, but '+' does not, because '++' and '+=' are valid longer tokens
    // that begin with '+'.
    //
    // The few token kinds satisfying these properties cover roughly 35--45%
    // of the tokens seen in practice.
    //
    // We represent the 'OneChar' kind with any positive value less than
    // TOK_LIMIT.  This representation lets us associate each one-char token
    // char16_t with a TokenKind and thus avoid a subsequent char16_t-to-TokenKind
    // conversion.
    OneChar_Min = 0,
    OneChar_Max = TOK_LIMIT - 1,

    Space = TOK_LIMIT,
    Ident,
    Dec,
    String,
    EOL,
    BasePrefix,
    Other,

    LastCharKind = Other
};

// OneChar: 40,  41,  44,  58,  59,  63,  91,  93,  123, 125, 126:
//          '(', ')', ',', ':', ';', '?', '[', ']', '{', '}', '~'
// Ident:   36, 65..90, 95, 97..122: '$', 'A'..'Z', '_', 'a'..'z'
// Dot:     46: '.'
// Equals:  61: '='
// String:  34, 39: '"', '\''
// Dec:     49..57: '1'..'9'
// Plus:    43: '+'
// BasePrefix:  48: '0'
// Space:   9, 11, 12, 32: '\t', '\v', '\f', ' '
// EOL:     10, 13: '\n', '\r'
//
#define T_COMMA     TOK_COMMA
#define T_COLON     TOK_COLON
#define T_BITNOT    TOK_BITNOT
#define Templat     String
#define _______     Other
static const uint8_t firstCharKinds[] = {
/*         0        1        2        3        4        5        6        7        8        9    */
/*   0+ */ _______, _______, _______, _______, _______, _______, _______, _______, _______,   Space,
/*  10+ */     EOL,   Space,   Space,     EOL, _______, _______, _______, _______, _______, _______,
/*  20+ */ _______, _______, _______, _______, _______, _______, _______, _______, _______, _______,
/*  30+ */ _______, _______,   Space, _______,  String, _______,   Ident, _______, _______,  String,
/*  40+ */  TOK_LP,  TOK_RP, _______, _______, T_COMMA,_______,  _______, _______,BasePrefix,  Dec,
/*  50+ */     Dec,     Dec,     Dec,     Dec,     Dec,     Dec,     Dec,    Dec,  T_COLON,TOK_SEMI,
/*  60+ */ _______, _______, _______,TOK_HOOK, _______,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  70+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  80+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/*  90+ */   Ident,  TOK_LB, _______,  TOK_RB, _______,   Ident, Templat,   Ident,   Ident,   Ident,
/* 100+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/* 110+ */   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,   Ident,
/* 120+ */   Ident,   Ident,   Ident,  TOK_LC, _______,  TOK_RC,T_BITNOT, _______
};
#undef T_COMMA
#undef T_COLON
#undef T_BITNOT
#undef Templat
#undef _______

static_assert(LastCharKind < (1 << (sizeof(firstCharKinds[0]) * 8)),
              "Elements of firstCharKinds[] are too small");

bool
TokenStream::getTokenInternal(TokenKind* ttp, Modifier modifier)
{
    int c, qc;
    Token* tp;
    FirstCharKind c1kind;
    const char16_t* numStart;
    bool hasExp;
    DecimalPoint decimalPoint;
    const char16_t* identStart;
    bool hadUnicodeEscape;

    // Check if in the middle of a template string. Have to get this out of
    // the way first.
    if (MOZ_UNLIKELY(modifier == TemplateTail)) {
        if (!getStringOrTemplateToken('`', &tp))
            goto error;
        goto out;
    }

  retry:
    if (MOZ_UNLIKELY(!userbuf.hasRawChars())) {
        tp = newToken(0);
        tp->type = TOK_EOF;
        flags.isEOF = true;
        goto out;
    }

    c = userbuf.getRawChar();
    MOZ_ASSERT(c != EOF);

    // Chars not in the range 0..127 are rare.  Getting them out of the way
    // early allows subsequent checking to be faster.
    if (MOZ_UNLIKELY(c >= 128)) {
        if (IsSpaceOrBOM2(c)) {
            if (c == LINE_SEPARATOR || c == PARA_SEPARATOR) {
                updateLineInfoForEOL();
                updateFlagsForEOL();
            }

            goto retry;
        }

        tp = newToken(-1);

        static_assert('$' < 128,
                      "IdentifierStart contains '$', but as !IsLetter('$'), "
                      "ensure that '$' is never handled here");
        static_assert('_' < 128,
                      "IdentifierStart contains '_', but as !IsLetter('_'), "
                      "ensure that '_' is never handled here");
        if (IsLetter(c)) {
            identStart = userbuf.addressOfNextRawChar() - 1;
            hadUnicodeEscape = false;
            goto identifier;
        }

        goto badchar;
    }

    // Get the token kind, based on the first char.  The ordering of c1kind
    // comparison is based on the frequency of tokens in real code -- Parsemark
    // (which represents typical JS code on the web) and the Unreal demo (which
    // represents asm.js code).
    //
    //                  Parsemark   Unreal
    //  OneChar         32.9%       39.7%
    //  Space           25.0%        0.6%
    //  Ident           19.2%       36.4%
    //  Dec              7.2%        5.1%
    //  String           7.9%        0.0%
    //  EOL              1.7%        0.0%
    //  BasePrefix       0.4%        4.9%
    //  Other            5.7%       13.3%
    //
    // The ordering is based mostly only Parsemark frequencies, with Unreal
    // frequencies used to break close categories (e.g. |Dec| and |String|).
    // |Other| is biggish, but no other token kind is common enough for it to
    // be worth adding extra values to FirstCharKind.
    //
    c1kind = FirstCharKind(firstCharKinds[c]);

    // Look for an unambiguous single-char token.
    //
    if (c1kind <= OneChar_Max) {
        tp = newToken(-1);
        tp->type = TokenKind(c1kind);
        goto out;
    }

    // Skip over non-EOL whitespace chars.
    //
    if (c1kind == Space)
        goto retry;

    // Look for an identifier.
    //
    if (c1kind == Ident) {
        tp = newToken(-1);
        identStart = userbuf.addressOfNextRawChar() - 1;
        hadUnicodeEscape = false;

      identifier:
        for (;;) {
            c = getCharIgnoreEOL();
            if (c == EOF)
                break;
            if (!IsIdentifierPart(c)) {
                if (c != '\\' || !matchUnicodeEscapeIdent(&qc))
                    break;
                hadUnicodeEscape = true;
            }
        }
        ungetCharIgnoreEOL(c);

        // Identifiers containing no Unicode escapes can be processed directly
        // from userbuf.  The rest must use the escapes converted via tokenbuf
        // before atomizing.
        const char16_t* chars;
        size_t length;
        if (hadUnicodeEscape) {
            if (!putIdentInTokenbuf(identStart))
                goto error;

            chars = tokenbuf.begin();
            length = tokenbuf.length();
        } else {
            chars = identStart;
            length = userbuf.addressOfNextRawChar() - identStart;
        }

        // Represent keywords as keyword tokens unless told otherwise.
        if (modifier != KeywordIsName) {
            if (const KeywordInfo* kw = FindKeyword(chars, length)) {
                // That said, keywords can't contain escapes.  (Contexts where
                // keywords are treated as names, that also sometimes treat
                // keywords as keywords, must manually check this requirement.)
                if (hadUnicodeEscape) {
                    reportError(JSMSG_ESCAPED_KEYWORD);
                    goto error;
                }

                tp->type = TOK_NAME;
                if (!checkForKeyword(kw, &tp->type))
                    goto error;
                if (tp->type != TOK_NAME)
                    goto out;
            }
        }

        JSAtom* atom = AtomizeChars(cx, chars, length);
        if (!atom)
            goto error;
        tp->type = TOK_NAME;
        tp->setName(atom->asPropertyName());
        goto out;
    }

    // Look for a decimal number.
    //
    if (c1kind == Dec) {
        tp = newToken(-1);
        numStart = userbuf.addressOfNextRawChar() - 1;

      decimal:
        decimalPoint = NoDecimal;
        hasExp = false;
        while (JS7_ISDEC(c))
            c = getCharIgnoreEOL();

        if (c == '.') {
            decimalPoint = HasDecimal;
          decimal_dot:
            do {
                c = getCharIgnoreEOL();
            } while (JS7_ISDEC(c));
        }
        if (c == 'e' || c == 'E') {
            hasExp = true;
            c = getCharIgnoreEOL();
            if (c == '+' || c == '-')
                c = getCharIgnoreEOL();
            if (!JS7_ISDEC(c)) {
                ungetCharIgnoreEOL(c);
                reportError(JSMSG_MISSING_EXPONENT);
                goto error;
            }
            do {
                c = getCharIgnoreEOL();
            } while (JS7_ISDEC(c));
        }
        ungetCharIgnoreEOL(c);

        if (c != EOF && IsIdentifierStart(c)) {
            reportError(JSMSG_IDSTART_AFTER_NUMBER);
            goto error;
        }

        // Unlike identifiers and strings, numbers cannot contain escaped
        // chars, so we don't need to use tokenbuf.  Instead we can just
        // convert the char16_t characters in userbuf to the numeric value.
        double dval;
        if (!((decimalPoint == HasDecimal) || hasExp)) {
            if (!GetDecimalInteger(cx, numStart, userbuf.addressOfNextRawChar(), &dval))
                goto error;
        } else {
            const char16_t* dummy;
            if (!js_strtod(cx, numStart, userbuf.addressOfNextRawChar(), &dummy, &dval))
                goto error;
        }
        tp->type = TOK_NUMBER;
        tp->setNumber(dval, decimalPoint);
        goto out;
    }

    // Look for a string or a template string.
    //
    if (c1kind == String) {
        if (!getStringOrTemplateToken(c, &tp))
            goto error;
        goto out;
    }

    // Skip over EOL chars, updating line state along the way.
    //
    if (c1kind == EOL) {
        // If it's a \r\n sequence: treat as a single EOL, skip over the \n.
        if (c == '\r' && userbuf.hasRawChars())
            userbuf.matchRawChar('\n');
        updateLineInfoForEOL();
        updateFlagsForEOL();
        goto retry;
    }

    // Look for a hexadecimal, octal, or binary number.
    //
    if (c1kind == BasePrefix) {
        tp = newToken(-1);
        int radix;
        c = getCharIgnoreEOL();
        if (c == 'x' || c == 'X') {
            radix = 16;
            c = getCharIgnoreEOL();
            if (!JS7_ISHEX(c)) {
                ungetCharIgnoreEOL(c);
                reportError(JSMSG_MISSING_HEXDIGITS);
                goto error;
            }
            numStart = userbuf.addressOfNextRawChar() - 1;  // one past the '0x'
            while (JS7_ISHEX(c))
                c = getCharIgnoreEOL();
        } else if (c == 'b' || c == 'B') {
            radix = 2;
            c = getCharIgnoreEOL();
            if (c != '0' && c != '1') {
                ungetCharIgnoreEOL(c);
                reportError(JSMSG_MISSING_BINARY_DIGITS);
                goto error;
            }
            numStart = userbuf.addressOfNextRawChar() - 1;  // one past the '0b'
            while (c == '0' || c == '1')
                c = getCharIgnoreEOL();
        } else if (c == 'o' || c == 'O') {
            radix = 8;
            c = getCharIgnoreEOL();
            if (c < '0' || c > '7') {
                ungetCharIgnoreEOL(c);
                reportError(JSMSG_MISSING_OCTAL_DIGITS);
                goto error;
            }
            numStart = userbuf.addressOfNextRawChar() - 1;  // one past the '0o'
            while ('0' <= c && c <= '7')
                c = getCharIgnoreEOL();
        } else if (JS7_ISDEC(c)) {
            radix = 8;
            numStart = userbuf.addressOfNextRawChar() - 1;  // one past the '0'
            while (JS7_ISDEC(c)) {
                // Octal integer literals are not permitted in strict mode code.
                if (!reportStrictModeError(JSMSG_DEPRECATED_OCTAL))
                    goto error;

                // Outside strict mode, we permit 08 and 09 as decimal numbers,
                // which makes our behaviour a superset of the ECMA numeric
                // grammar. We might not always be so permissive, so we warn
                // about it.
                if (c >= '8') {
                    if (!reportWarning(JSMSG_BAD_OCTAL, c == '8' ? "08" : "09")) {
                        goto error;
                    }
                    goto decimal;   // use the decimal scanner for the rest of the number
                }
                c = getCharIgnoreEOL();
            }
        } else {
            // '0' not followed by 'x', 'X' or a digit;  scan as a decimal number.
            numStart = userbuf.addressOfNextRawChar() - 1;
            goto decimal;
        }
        ungetCharIgnoreEOL(c);

        if (c != EOF && IsIdentifierStart(c)) {
            reportError(JSMSG_IDSTART_AFTER_NUMBER);
            goto error;
        }

        double dval;
        const char16_t* dummy;
        if (!GetPrefixInteger(cx, numStart, userbuf.addressOfNextRawChar(), radix, &dummy, &dval))
            goto error;
        tp->type = TOK_NUMBER;
        tp->setNumber(dval, NoDecimal);
        goto out;
    }

    // This handles everything else.
    //
    MOZ_ASSERT(c1kind == Other);
    tp = newToken(-1);
    switch (c) {
      case '.':
        c = getCharIgnoreEOL();
        if (JS7_ISDEC(c)) {
            numStart = userbuf.addressOfNextRawChar() - 2;
            decimalPoint = HasDecimal;
            hasExp = false;
            goto decimal_dot;
        }
        if (c == '.') {
            if (matchChar('.')) {
                tp->type = TOK_TRIPLEDOT;
                goto out;
            }
        }
        ungetCharIgnoreEOL(c);
        tp->type = TOK_DOT;
        goto out;

      case '=':
        if (matchChar('='))
            tp->type = matchChar('=') ? TOK_STRICTEQ : TOK_EQ;
        else if (matchChar('>'))
            tp->type = TOK_ARROW;
        else
            tp->type = TOK_ASSIGN;
        goto out;

      case '+':
        if (matchChar('+'))
            tp->type = TOK_INC;
        else
            tp->type = matchChar('=') ? TOK_ADDASSIGN : TOK_ADD;
        goto out;

      case '\\':
        hadUnicodeEscape = matchUnicodeEscapeIdStart(&qc);
        if (hadUnicodeEscape) {
            identStart = userbuf.addressOfNextRawChar() - 6;
            goto identifier;
        }
        goto badchar;

      case '|':
        if (matchChar('|'))
            tp->type = TOK_OR;
        else
            tp->type = matchChar('=') ? TOK_BITORASSIGN : TOK_BITOR;
        goto out;

      case '^':
        tp->type = matchChar('=') ? TOK_BITXORASSIGN : TOK_BITXOR;
        goto out;

      case '&':
        if (matchChar('&'))
            tp->type = TOK_AND;
        else
            tp->type = matchChar('=') ? TOK_BITANDASSIGN : TOK_BITAND;
        goto out;

      case '!':
        if (matchChar('='))
            tp->type = matchChar('=') ? TOK_STRICTNE : TOK_NE;
        else
            tp->type = TOK_NOT;
        goto out;

      case '<':
        // NB: treat HTML begin-comment as comment-till-end-of-line.
        if (matchChar('!')) {
            if (matchChar('-')) {
                if (matchChar('-'))
                    goto skipline;
                ungetChar('-');
            }
            ungetChar('!');
        }
        if (matchChar('<')) {
            tp->type = matchChar('=') ? TOK_LSHASSIGN : TOK_LSH;
        } else {
            tp->type = matchChar('=') ? TOK_LE : TOK_LT;
        }
        goto out;

      case '>':
        if (matchChar('>')) {
            if (matchChar('>'))
                tp->type = matchChar('=') ? TOK_URSHASSIGN : TOK_URSH;
            else
                tp->type = matchChar('=') ? TOK_RSHASSIGN : TOK_RSH;
        } else {
            tp->type = matchChar('=') ? TOK_GE : TOK_GT;
        }
        goto out;

      case '*':
#ifdef JS_HAS_EXPONENTIATION
        if (matchChar('*'))
            tp->type = matchChar('=') ? TOK_POWASSIGN : TOK_POW;
        else
#endif
            tp->type = matchChar('=') ? TOK_MULASSIGN : TOK_MUL;
        goto out;

      case '/':
        // Look for a single-line comment.
        if (matchChar('/')) {
            c = peekChar();
            if (c == '@' || c == '#') {
                bool shouldWarn = getChar() == '@';
                if (!getDirectives(false, shouldWarn))
                    goto error;
            }

        skipline:
            while ((c = getChar()) != EOF && c != '\n')
                continue;
            ungetChar(c);
            cursor = (cursor - 1) & ntokensMask;
            goto retry;
        }

        // Look for a multi-line comment.
        if (matchChar('*')) {
            unsigned linenoBefore = lineno;
            while ((c = getChar()) != EOF &&
                   !(c == '*' && matchChar('/'))) {
                if (c == '@' || c == '#') {
                    bool shouldWarn = c == '@';
                    if (!getDirectives(true, shouldWarn))
                        goto error;
                }
            }
            if (c == EOF) {
                reportError(JSMSG_UNTERMINATED_COMMENT);
                goto error;
            }
            if (linenoBefore != lineno)
                updateFlagsForEOL();
            cursor = (cursor - 1) & ntokensMask;
            goto retry;
        }

        // Look for a regexp.
        if (modifier == Operand) {
            tokenbuf.clear();

            bool inCharClass = false;
            for (;;) {
                c = getChar();
                if (c == '\\') {
                    if (!tokenbuf.append(c))
                        goto error;
                    c = getChar();
                } else if (c == '[') {
                    inCharClass = true;
                } else if (c == ']') {
                    inCharClass = false;
                } else if (c == '/' && !inCharClass) {
                    // For compat with IE, allow unescaped / in char classes.
                    break;
                }
                if (c == '\n' || c == EOF) {
                    ungetChar(c);
                    reportError(JSMSG_UNTERMINATED_REGEXP);
                    goto error;
                }
                if (!tokenbuf.append(c))
                    goto error;
            }

            RegExpFlag reflags = NoFlags;
            unsigned length = tokenbuf.length() + 1;
            while (true) {
                c = peekChar();
                if (c == 'g' && !(reflags & GlobalFlag))
                    reflags = RegExpFlag(reflags | GlobalFlag);
                else if (c == 'i' && !(reflags & IgnoreCaseFlag))
                    reflags = RegExpFlag(reflags | IgnoreCaseFlag);
                else if (c == 'm' && !(reflags & MultilineFlag))
                    reflags = RegExpFlag(reflags | MultilineFlag);
                else if (c == 'y' && !(reflags & StickyFlag))
                    reflags = RegExpFlag(reflags | StickyFlag);
                else
                    break;
                getChar();
                length++;
            }

            c = peekChar();
            if (JS7_ISLET(c)) {
                char buf[2] = { '\0', '\0' };
                tp->pos.begin += length + 1;
                buf[0] = char(c);
                reportError(JSMSG_BAD_REGEXP_FLAG, buf);
                (void) getChar();
                goto error;
            }
            tp->type = TOK_REGEXP;
            tp->setRegExpFlags(reflags);
            goto out;
        }

        tp->type = matchChar('=') ? TOK_DIVASSIGN : TOK_DIV;
        goto out;

      case '%':
        tp->type = matchChar('=') ? TOK_MODASSIGN : TOK_MOD;
        goto out;

      case '-':
        if (matchChar('-')) {
            if (peekChar() == '>' && !flags.isDirtyLine)
                goto skipline;
            tp->type = TOK_DEC;
        } else {
            tp->type = matchChar('=') ? TOK_SUBASSIGN : TOK_SUB;
        }
        goto out;

      badchar:
      default:
        reportError(JSMSG_ILLEGAL_CHARACTER);
        goto error;
    }

    MOZ_CRASH("should have jumped to |out| or |error|");

  out:
    if (flags.hitOOM)
        return reportError(JSMSG_OUT_OF_MEMORY);

    flags.isDirtyLine = true;
    tp->pos.end = userbuf.offset();
#ifdef DEBUG
    // Save the modifier used to get this token, so that if an ungetToken()
    // occurs and then the token is re-gotten (or peeked, etc.), we can assert
    // that both gets have used the same modifiers.
    tp->modifier = modifier;
    tp->modifierException = NoException;
#endif
    MOZ_ASSERT(IsTokenSane(tp));
    *ttp = tp->type;
    return true;

  error:
    if (flags.hitOOM)
        return reportError(JSMSG_OUT_OF_MEMORY);

    flags.isDirtyLine = true;
    tp->pos.end = userbuf.offset();
    MOZ_MAKE_MEM_UNDEFINED(&tp->type, sizeof(tp->type));
    flags.hadError = true;
#ifdef DEBUG
    // Poisoning userbuf on error establishes an invariant: once an erroneous
    // token has been seen, userbuf will not be consulted again.  This is true
    // because the parser will deal with the illegal token by aborting parsing
    // immediately.
    userbuf.poison();
#endif
    MOZ_MAKE_MEM_UNDEFINED(ttp, sizeof(*ttp));
    return false;
}

bool
TokenStream::getBracedUnicode(uint32_t* cp)
{
    consumeKnownChar('{');

    bool first = true;
    int32_t c;
    uint32_t code = 0;
    while (true) {
        c = getCharIgnoreEOL();
        if (c == EOF)
            return false;
        if (c == '}') {
            if (first)
                return false;
            break;
        }

        if (!JS7_ISHEX(c))
            return false;

        code = (code << 4) | JS7_UNHEX(c);
        if (code > 0x10FFFF)
            return false;
        first = false;
    }

    *cp = code;
    return true;
}

bool
TokenStream::getStringOrTemplateToken(int untilChar, Token** tp)
{
    int c;
    int nc = -1;

    bool parsingTemplate = (untilChar == '`');

    *tp = newToken(-1);
    tokenbuf.clear();

    // We need to detect any of these chars:  " or ', \n (or its
    // equivalents), \\, EOF.  Because we detect EOL sequences here and
    // put them back immediately, we can use getCharIgnoreEOL().
    while ((c = getCharIgnoreEOL()) != untilChar) {
        if (c == EOF) {
            ungetCharIgnoreEOL(c);
            reportError(JSMSG_UNTERMINATED_STRING);
            return false;
        }

        if (c == '\\') {
            switch (c = getChar()) {
              case 'b': c = '\b'; break;
              case 'f': c = '\f'; break;
              case 'n': c = '\n'; break;
              case 'r': c = '\r'; break;
              case 't': c = '\t'; break;
              case 'v': c = '\v'; break;

              case '\n':
                // ES5 7.8.4: an escaped line terminator represents
                // no character.
                continue;

              // Unicode character specification.
              case 'u': {
                if (peekChar() == '{') {
                    uint32_t code;
                    if (!getBracedUnicode(&code)) {
                        reportError(JSMSG_MALFORMED_ESCAPE, "Unicode");
                        return false;
                    }

                    MOZ_ASSERT(code <= 0x10FFFF);
                    if (code < 0x10000) {
                        c = code;
                    } else {
                        if (!tokenbuf.append((code - 0x10000) / 1024 + 0xD800))
                            return false;
                        c = ((code - 0x10000) % 1024) + 0xDC00;
                    }
                    break;
                }

                char16_t cp[4];
                if (peekChars(4, cp) &&
                    JS7_ISHEX(cp[0]) && JS7_ISHEX(cp[1]) && JS7_ISHEX(cp[2]) && JS7_ISHEX(cp[3]))
                {
                    c = JS7_UNHEX(cp[0]);
                    c = (c << 4) + JS7_UNHEX(cp[1]);
                    c = (c << 4) + JS7_UNHEX(cp[2]);
                    c = (c << 4) + JS7_UNHEX(cp[3]);
                    skipChars(4);
                } else {
                    reportError(JSMSG_MALFORMED_ESCAPE, "Unicode");
                    return false;
                }
                break;
              }

              // Hexadecimal character specification.
              case 'x': {
                char16_t cp[2];
                if (peekChars(2, cp) && JS7_ISHEX(cp[0]) && JS7_ISHEX(cp[1])) {
                    c = (JS7_UNHEX(cp[0]) << 4) + JS7_UNHEX(cp[1]);
                    skipChars(2);
                } else {
                    reportError(JSMSG_MALFORMED_ESCAPE, "hexadecimal");
                    return false;
                }
                break;
              }

              default:
                // Octal character specification.
                if (JS7_ISOCT(c)) {
                    int32_t val = JS7_UNOCT(c);

                    c = peekChar();

                    // Strict mode code allows only \0, then a non-digit.
                    if (val != 0 || JS7_ISDEC(c)) {
                        if (parsingTemplate) {
                            reportError(JSMSG_DEPRECATED_OCTAL);
                            return false;
                        }
                        if (!reportStrictModeError(JSMSG_DEPRECATED_OCTAL))
                            return false;
                        flags.sawOctalEscape = true;
                    }

                    if (JS7_ISOCT(c)) {
                        val = 8 * val + JS7_UNOCT(c);
                        getChar();
                        c = peekChar();
                        if (JS7_ISOCT(c)) {
                            int32_t save = val;
                            val = 8 * val + JS7_UNOCT(c);
                            if (val <= 0xFF)
                                getChar();
                            else
                                val = save;
                        }
                    }

                    c = char16_t(val);
                }
                break;
            }
        } else if (TokenBuf::isRawEOLChar(c)) {
            if (!parsingTemplate) {
                ungetCharIgnoreEOL(c);
                reportError(JSMSG_UNTERMINATED_STRING);
                return false;
            }
            if (c == '\r') {
                c = '\n';
                if (userbuf.peekRawChar() == '\n')
                    skipChars(1);
            }
            updateLineInfoForEOL();
            updateFlagsForEOL();
        } else if (parsingTemplate && c == '$') {
            if ((nc = getCharIgnoreEOL()) == '{')
                break;
            ungetCharIgnoreEOL(nc);
        }

        if (!tokenbuf.append(c)) {
            ReportOutOfMemory(cx);
            return false;
        }
    }

    JSAtom* atom = atomize(cx, tokenbuf);
    if (!atom)
        return false;

    if (!parsingTemplate) {
        (*tp)->type = TOK_STRING;
    } else {
        if (c == '$' && nc == '{')
            (*tp)->type = TOK_TEMPLATE_HEAD;
        else
            (*tp)->type = TOK_NO_SUBS_TEMPLATE;
    }

    (*tp)->setAtom(atom);
    return true;
}

JS_FRIEND_API(int)
js_fgets(char* buf, int size, FILE* file)
{
    int n, i, c;
    bool crflag;

    n = size - 1;
    if (n < 0)
        return -1;

    crflag = false;
    for (i = 0; i < n && (c = fast_getc(file)) != EOF; i++) {
        buf[i] = c;
        if (c == '\n') {        // any \n ends a line
            i++;                // keep the \n; we know there is room for \0
            break;
        }
        if (crflag) {           // \r not followed by \n ends line at the \r
            ungetc(c, file);
            break;              // and overwrite c in buf with \0
        }
        crflag = (c == '\r');
    }

    buf[i] = '\0';
    return i;
}

const char*
frontend::TokenKindToDesc(TokenKind tt)
{
    switch (tt) {
#define EMIT_CASE(name, desc) case TOK_##name: return desc;
      FOR_EACH_TOKEN_KIND(EMIT_CASE)
#undef EMIT_CASE
      case TOK_LIMIT:
        MOZ_ASSERT_UNREACHABLE("TOK_LIMIT should not be passed.");
        break;
    }

    return "<bad TokenKind>";
}

#ifdef DEBUG
const char*
TokenKindToString(TokenKind tt)
{
    switch (tt) {
#define EMIT_CASE(name, desc) case TOK_##name: return "TOK_" #name;
      FOR_EACH_TOKEN_KIND(EMIT_CASE)
#undef EMIT_CASE
      case TOK_LIMIT: break;
    }

    return "<bad TokenKind>";
}
#endif
