/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "irregexp/RegExpParser.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Move.h"

#include "frontend/TokenStream.h"
#include "gc/GC.h"
#include "irregexp/RegExpCharacters.h"
#include "util/StringBuffer.h"
#include "vm/ErrorReporting.h"

using namespace js;
using namespace js::irregexp;

using mozilla::Move;
using mozilla::PointerRangeSize;

// ----------------------------------------------------------------------------
// RegExpBuilder

RegExpBuilder::RegExpBuilder(LifoAlloc* alloc)
  : alloc(alloc),
    pending_empty_(false),
    characters_(nullptr)
#ifdef DEBUG
  , last_added_(ADD_NONE)
#endif
{}

void
RegExpBuilder::FlushCharacters()
{
    pending_empty_ = false;
    if (characters_ != nullptr) {
        RegExpTree* atom = alloc->newInfallible<RegExpAtom>(characters_);
        characters_ = nullptr;
        text_.Add(alloc, atom);
#ifdef DEBUG
        last_added_ = ADD_ATOM;
#endif
    }
}

void
RegExpBuilder::FlushText()
{
    FlushCharacters();
    int num_text = text_.length();
    if (num_text == 0)
        return;
    if (num_text == 1) {
        terms_.Add(alloc, text_.last());
    } else {
        RegExpText* text = alloc->newInfallible<RegExpText>(alloc);
        for (int i = 0; i < num_text; i++)
            text_.Get(i)->AppendToText(text);
        terms_.Add(alloc, text);
    }
    text_.Clear();
}

void
RegExpBuilder::AddCharacter(char16_t c)
{
    pending_empty_ = false;
    if (characters_ == nullptr)
        characters_ = alloc->newInfallible<CharacterVector>(*alloc);
    characters_->append(c);
#ifdef DEBUG
    last_added_ = ADD_CHAR;
#endif
}

void
RegExpBuilder::AddEmpty()
{
    pending_empty_ = true;
}

void
RegExpBuilder::AddAtom(RegExpTree* term)
{
    if (term->IsEmpty()) {
        AddEmpty();
        return;
    }
    if (term->IsTextElement()) {
        FlushCharacters();
        text_.Add(alloc, term);
    } else {
        FlushText();
        terms_.Add(alloc, term);
    }
#ifdef DEBUG
    last_added_ = ADD_ATOM;
#endif
}

void
RegExpBuilder::AddAssertion(RegExpTree* assert)
{
    FlushText();
    if (terms_.length() > 0 && terms_.last()->IsAssertion()) {
        // Omit repeated assertions of the same type.
        RegExpAssertion* last = terms_.last()->AsAssertion();
        RegExpAssertion* next = assert->AsAssertion();
        if (last->assertion_type() == next->assertion_type()) return;
    }
    terms_.Add(alloc, assert);
#ifdef DEBUG
    last_added_ = ADD_ASSERT;
#endif
}

void
RegExpBuilder::NewAlternative()
{
    FlushTerms();
}

void
RegExpBuilder::FlushTerms()
{
    FlushText();
    int num_terms = terms_.length();
    RegExpTree* alternative;
    if (num_terms == 0)
        alternative = RegExpEmpty::GetInstance();
    else if (num_terms == 1)
        alternative = terms_.last();
    else
        alternative = alloc->newInfallible<RegExpAlternative>(terms_.GetList(alloc));
    alternatives_.Add(alloc, alternative);
    terms_.Clear();
#ifdef DEBUG
    last_added_ = ADD_NONE;
#endif
}

RegExpTree*
RegExpBuilder::ToRegExp()
{
    FlushTerms();
    int num_alternatives = alternatives_.length();
    if (num_alternatives == 0) {
        return RegExpEmpty::GetInstance();
    }
    if (num_alternatives == 1) {
        return alternatives_.last();
    }
    return alloc->newInfallible<RegExpDisjunction>(alternatives_.GetList(alloc));
}

void
RegExpBuilder::AddQuantifierToAtom(int min, int max,
                                   RegExpQuantifier::QuantifierType quantifier_type)
{
    if (pending_empty_) {
        pending_empty_ = false;
        return;
    }
    RegExpTree* atom;
    if (characters_ != nullptr) {
        MOZ_ASSERT(last_added_ == ADD_CHAR);
        // Last atom was character.
        CharacterVector* char_vector = characters_;
        int num_chars = char_vector->length();
        if (num_chars > 1) {
            CharacterVector* prefix = alloc->newInfallible<CharacterVector>(*alloc);
            prefix->append(char_vector->begin(), num_chars - 1);
            text_.Add(alloc, alloc->newInfallible<RegExpAtom>(prefix));
            char_vector = alloc->newInfallible<CharacterVector>(*alloc);
            char_vector->append((*characters_)[num_chars - 1]);
        }
        characters_ = nullptr;
        atom = alloc->newInfallible<RegExpAtom>(char_vector);
        FlushText();
    } else if (text_.length() > 0) {
        MOZ_ASSERT(last_added_ == ADD_ATOM);
        atom = text_.RemoveLast();
        FlushText();
    } else if (terms_.length() > 0) {
        MOZ_ASSERT(last_added_ == ADD_ATOM);
        atom = terms_.RemoveLast();
        if (atom->max_match() == 0) {
            // Guaranteed to only match an empty string.
#ifdef DEBUG
            last_added_ = ADD_TERM;
#endif
            if (min == 0)
                return;
            terms_.Add(alloc, atom);
            return;
        }
    } else {
        // Only call immediately after adding an atom or character!
        MOZ_CRASH("Bad call");
    }
    terms_.Add(alloc, alloc->newInfallible<RegExpQuantifier>(min, max, quantifier_type, atom));
#ifdef DEBUG
    last_added_ = ADD_TERM;
#endif
}

// ----------------------------------------------------------------------------
// RegExpParser

template <typename CharT>
RegExpParser<CharT>::RegExpParser(frontend::TokenStreamAnyChars& ts, LifoAlloc* alloc,
                                  const CharT* chars, const CharT* end, bool multiline_mode,
                                  bool unicode, bool ignore_case)
  : ts(ts),
    alloc(alloc),
    captures_(nullptr),
    start_(chars),
    next_pos_(start_),
    end_(end),
    current_(kEndMarker),
    capture_count_(0),
    has_more_(true),
    multiline_(multiline_mode),
    unicode_(unicode),
    ignore_case_(ignore_case),
    simple_(false),
    contains_anchor_(false),
    is_scanned_for_captures_(false)
{
    Advance();
}

template <typename CharT>
void
RegExpParser<CharT>::SyntaxError(unsigned errorNumber, ...)
{
    ErrorMetadata err;

    ts.fillExcludingContext(&err, ts.currentToken().pos.begin);

    // For most error reporting, the line of context derives from the token
    // stream.  So when location information doesn't come from the token
    // stream, we can't give a line of context.  But here the "line of context"
    // can be (and is) derived from the pattern text, so we can provide it no
    // matter if the location is derived from the caller.
    size_t offset = PointerRangeSize(start_, next_pos_ - 1);
    size_t end = PointerRangeSize(start_, end_);

    const CharT* windowStart = (offset > ErrorMetadata::lineOfContextRadius)
                               ? start_ + (offset - ErrorMetadata::lineOfContextRadius)
                               : start_;

    const CharT* windowEnd = (end - offset > ErrorMetadata::lineOfContextRadius)
                             ? start_ + offset + ErrorMetadata::lineOfContextRadius
                             : end_;

    size_t windowLength = PointerRangeSize(windowStart, windowEnd);
    MOZ_ASSERT(windowLength <= ErrorMetadata::lineOfContextRadius * 2);

    // Create the windowed string, not including the potential line
    // terminator.
    StringBuffer windowBuf(ts.context());
    if (!windowBuf.append(windowStart, windowEnd))
        return;

    // The line of context must be null-terminated, and StringBuffer doesn't
    // make that happen unless we force it to.
    if (!windowBuf.append('\0'))
        return;

    err.lineOfContext.reset(windowBuf.stealChars());
    if (!err.lineOfContext)
        return;

    err.lineLength = windowLength;
    err.tokenOffset = offset - (windowStart - start_);

    va_list args;
    va_start(args, errorNumber);

    ReportCompileError(ts.context(), Move(err), nullptr, JSREPORT_ERROR, errorNumber, args);

    va_end(args);
}

template <typename CharT>
RegExpTree*
RegExpParser<CharT>::ReportError(unsigned errorNumber, const char* param /* = nullptr */)
{
    gc::AutoSuppressGC suppressGC(ts.context());
    SyntaxError(errorNumber, param);
    return nullptr;
}

template <typename CharT>
void
RegExpParser<CharT>::Advance()
{
    if (next_pos_ < end_) {
        current_ = *next_pos_;
        next_pos_++;
    } else {
        current_ = kEndMarker;
        next_pos_ = end_ + 1;
        has_more_ = false;
    }
}

// Returns the value (0 .. 15) of a hexadecimal character c.
// If c is not a legal hexadecimal character, returns a value < 0.
inline int
HexValue(uint32_t c)
{
    c -= '0';
    if (static_cast<unsigned>(c) <= 9) return c;
    c = (c | 0x20) - ('a' - '0');  // detect 0x11..0x16 and 0x31..0x36.
    if (static_cast<unsigned>(c) <= 5) return c + 10;
    return -1;
}

template <typename CharT>
widechar
RegExpParser<CharT>::ParseOctalLiteral()
{
    MOZ_ASSERT('0' <= current() && current() <= '7');
    // For compatibility with some other browsers (not all), we parse
    // up to three octal digits with a value below 256.
    widechar value = current() - '0';
    Advance();
    if ('0' <= current() && current() <= '7') {
        value = value * 8 + current() - '0';
        Advance();
        if (value < 32 && '0' <= current() && current() <= '7') {
            value = value * 8 + current() - '0';
            Advance();
        }
    }
    return value;
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseHexEscape(int length, widechar* value)
{
    const CharT* start = position();
    uint32_t val = 0;
    bool done = false;
    for (int i = 0; !done; i++) {
        widechar c = current();
        int d = HexValue(c);
        if (d < 0) {
            Reset(start);
            return false;
        }
        val = val * 16 + d;
        Advance();
        if (i == length - 1) {
            done = true;
        }
    }
    *value = val;
    return true;
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseBracedHexEscape(widechar* value)
{
    MOZ_ASSERT(current() == '{');
    Advance();

    bool first = true;
    uint32_t code = 0;
    while (true) {
        widechar c = current();
        if (c == kEndMarker) {
            ReportError(JSMSG_INVALID_UNICODE_ESCAPE);
            return false;
        }
        if (c == '}') {
            if (first) {
                ReportError(JSMSG_INVALID_UNICODE_ESCAPE);
                return false;
            }
            Advance();
            break;
        }

        int d = HexValue(c);
        if (d < 0) {
            ReportError(JSMSG_INVALID_UNICODE_ESCAPE);
            return false;
        }
        code = (code << 4) | d;
        if (code > unicode::NonBMPMax) {
            ReportError(JSMSG_UNICODE_OVERFLOW, "regular expression");
            return false;
        }
        Advance();
        first = false;
    }

    *value = code;
    return true;
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseTrailSurrogate(widechar* value)
{
    if (current() != '\\')
        return false;

    const CharT* start = position();
    Advance();
    if (current() != 'u') {
        Reset(start);
        return false;
    }
    Advance();
    if (!ParseHexEscape(4, value)) {
        Reset(start);
        return false;
    }
    if (!unicode::IsTrailSurrogate(*value)) {
        Reset(start);
        return false;
    }
    return true;
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseRawSurrogatePair(char16_t* lead, char16_t* trail)
{
    widechar c1 = current();
    if (!unicode::IsLeadSurrogate(c1))
        return false;

    const CharT* start = position();
    Advance();
    widechar c2 = current();
    if (!unicode::IsTrailSurrogate(c2)) {
        Reset(start);
        return false;
    }
    Advance();
    *lead = c1;
    *trail = c2;
    return true;
}

static inline RegExpTree*
RangeAtom(LifoAlloc* alloc, char16_t from, char16_t to)
{
    CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    ranges->append(CharacterRange::Range(from, to));
    return alloc->newInfallible<RegExpCharacterClass>(ranges, false);
}

static inline RegExpTree*
NegativeLookahead(LifoAlloc* alloc, char16_t from, char16_t to)
{
    return alloc->newInfallible<RegExpLookahead>(RangeAtom(alloc, from, to), false, 0, 0);
}

static bool
IsSyntaxCharacter(widechar c)
{
  switch (c) {
    case '^':
    case '$':
    case '\\':
    case '.':
    case '*':
    case '+':
    case '?':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '|':
    case '/':
      return true;
    default:
      return false;
  }
}

inline bool
IsInRange(int value, int lower_limit, int higher_limit)
{
    MOZ_ASSERT(lower_limit <= higher_limit);
    return static_cast<unsigned int>(value - lower_limit) <=
           static_cast<unsigned int>(higher_limit - lower_limit);
}

inline bool
IsDecimalDigit(widechar c)
{
    // ECMA-262, 3rd, 7.8.3 (p 16)
    return IsInRange(c, '0', '9');
}

#ifdef DEBUG
// Currently only used in an assert.kASSERT.
static bool
IsSpecialClassEscape(widechar c)
{
  switch (c) {
    case 'd': case 'D':
    case 's': case 'S':
    case 'w': case 'W':
      return true;
    default:
      return false;
  }
}
#endif

template <typename CharT>
bool
RegExpParser<CharT>::ParseClassCharacterEscape(widechar* code)
{
    MOZ_ASSERT(current() == '\\');
    MOZ_ASSERT(has_next() && !IsSpecialClassEscape(Next()));
    Advance();
    switch (current()) {
      case 'b':
        Advance();
        *code = '\b';
        return true;
      // ControlEscape :: one of
      //   f n r t v
      case 'f':
        Advance();
        *code = '\f';
        return true;
      case 'n':
        Advance();
        *code = '\n';
        return true;
      case 'r':
        Advance();
        *code = '\r';
        return true;
      case 't':
        Advance();
        *code = '\t';
        return true;
      case 'v':
        Advance();
        *code = '\v';
        return true;
      case 'c': {
        widechar controlLetter = Next();
        widechar letter = controlLetter & ~('A' ^ 'a');
        // For compatibility with JSC, inside a character class
        // we also accept digits and underscore as control characters,
        // but only in non-unicode mode
        if ((!unicode_ &&
             ((controlLetter >= '0' && controlLetter <= '9') ||
              controlLetter == '_')) ||
            (letter >= 'A' && letter <= 'Z'))
        {
            Advance(2);
            // Control letters mapped to ASCII control characters in the range
            // 0x00-0x1f.
            *code = controlLetter & 0x1f;
            return true;
        }
        if (unicode_) {
            ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
            return false;
        }
        // We match JSC in reading the backslash as a literal
        // character instead of as starting an escape.
        *code = '\\';
        return true;
      }
      case '0':
        if (unicode_) {
            Advance();
            if (IsDecimalDigit(current()))
                return ReportError(JSMSG_INVALID_DECIMAL_ESCAPE);
            *code = 0;
            return true;
        }
        MOZ_FALLTHROUGH;
      case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        if (unicode_) {
            ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
            return false;
        }
        // For compatibility, outside of unicode mode, we interpret a decimal
        // escape that isn't a back reference (and therefore either \0 or not
        // valid according to the specification) as a 1..3 digit octal
        // character code.
        *code = ParseOctalLiteral();
        return true;
      case 'x': {
        Advance();
        widechar value;
        if (ParseHexEscape(2, &value)) {
            *code = value;
            return true;
        }
        if (unicode_) {
            ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
            return false;
        }
        // If \x is not followed by a two-digit hexadecimal, treat it
        // as an identity escape in non-unicode mode.
        *code = 'x';
        return true;
      }
      case 'u': {
        Advance();
        widechar value;
        if (unicode_) {
            if (current() == '{') {
                if (!ParseBracedHexEscape(&value))
                    return false;
                *code = value;
                return true;
            }
            if (ParseHexEscape(4, &value)) {
                if (unicode::IsLeadSurrogate(value)) {
                    widechar trail;
                    if (ParseTrailSurrogate(&trail)) {
                        *code = unicode::UTF16Decode(value, trail);
                        return true;
                    }
                }
                *code = value;
                return true;
            }
            ReportError(JSMSG_INVALID_UNICODE_ESCAPE);
            return false;
        }
        if (ParseHexEscape(4, &value)) {
            *code = value;
            return true;
        }
        // If \u is not followed by a four-digit or braced hexadecimal, treat it
        // as an identity escape.
        *code = 'u';
        return true;
      }
      default: {
        // Extended identity escape (non-unicode only). We accept any character
        // that hasn't been matched by a more specific case, not just the subset
        // required by the ECMAScript specification.
        widechar result = current();
        if (unicode_ && result != '-' && !IsSyntaxCharacter(result)) {
            ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
            return false;
        }
        Advance();
        *code = result;
        return true;
      }
    }
    return true;
}

class WideCharRange
{
  public:
    WideCharRange()
      : from_(0), to_(0)
    {}

    WideCharRange(widechar from, widechar to)
      : from_(from), to_(to)
    {}

    static inline WideCharRange Singleton(widechar value) {
        return WideCharRange(value, value);
    }
    static inline WideCharRange Range(widechar from, widechar to) {
        MOZ_ASSERT(from <= to);
        return WideCharRange(from, to);
    }

    bool Contains(widechar i) const { return from_ <= i && i <= to_; }
    widechar from() const { return from_; }
    widechar to() const { return to_; }

  private:
    widechar from_;
    widechar to_;
};

typedef InfallibleVector<WideCharRange, 1> WideCharRangeVector;

static inline CharacterRange
LeadSurrogateRange()
{
    return CharacterRange::Range(unicode::LeadSurrogateMin, unicode::LeadSurrogateMax);
}

static inline CharacterRange
TrailSurrogateRange()
{
    return CharacterRange::Range(unicode::TrailSurrogateMin, unicode::TrailSurrogateMax);
}

static inline WideCharRange
NonBMPRange()
{
    return WideCharRange::Range(unicode::NonBMPMin, unicode::NonBMPMax);
}

static const char16_t kNoCharClass = 0;

// Adds a character or pre-defined character class to character ranges.
// If char_class is not kInvalidClass, it's interpreted as a class
// escape (i.e., 's' means whitespace, from '\s').
static inline void
AddCharOrEscape(LifoAlloc* alloc,
                CharacterRangeVector* ranges,
                char16_t char_class,
                widechar c)
{
    if (char_class != kNoCharClass)
        CharacterRange::AddClassEscape(alloc, char_class, ranges);
    else
        ranges->append(CharacterRange::Singleton(c));
}

static inline void
AddCharOrEscapeUnicode(LifoAlloc* alloc,
                       CharacterRangeVector* ranges,
                       CharacterRangeVector* lead_ranges,
                       CharacterRangeVector* trail_ranges,
                       WideCharRangeVector* wide_ranges,
                       char16_t char_class,
                       widechar c,
                       bool ignore_case)
{
    if (char_class != kNoCharClass) {
        CharacterRange::AddClassEscapeUnicode(alloc, char_class, ranges, ignore_case);
        switch (char_class) {
          case 'S':
          case 'W':
          case 'D':
            lead_ranges->append(LeadSurrogateRange());
            trail_ranges->append(TrailSurrogateRange());
            wide_ranges->append(NonBMPRange());
            break;
          case '.':
            MOZ_CRASH("Bad char_class!");
        }
        return;
    }

    if (unicode::IsLeadSurrogate(c))
        lead_ranges->append(CharacterRange::Singleton(c));
    else if (unicode::IsTrailSurrogate(c))
        trail_ranges->append(CharacterRange::Singleton(c));
    else if (c >= unicode::NonBMPMin)
        wide_ranges->append(WideCharRange::Singleton(c));
    else
        ranges->append(CharacterRange::Singleton(c));
}

static inline void
AddUnicodeRange(LifoAlloc* alloc,
                CharacterRangeVector* ranges,
                CharacterRangeVector* lead_ranges,
                CharacterRangeVector* trail_ranges,
                WideCharRangeVector* wide_ranges,
                widechar first,
                widechar next)
{
    MOZ_ASSERT(first <= next);
    if (first < unicode::LeadSurrogateMin) {
        if (next < unicode::LeadSurrogateMin) {
            ranges->append(CharacterRange::Range(first, next));
            return;
        }
        ranges->append(CharacterRange::Range(first, unicode::LeadSurrogateMin - 1));
        first = unicode::LeadSurrogateMin;
    }
    if (first <= unicode::LeadSurrogateMax) {
        if (next <= unicode::LeadSurrogateMax) {
            lead_ranges->append(CharacterRange::Range(first, next));
            return;
        }
        lead_ranges->append(CharacterRange::Range(first, unicode::LeadSurrogateMax));
        first = unicode::LeadSurrogateMax + 1;
    }
    MOZ_ASSERT(unicode::LeadSurrogateMax + 1 == unicode::TrailSurrogateMin);
    if (first <= unicode::TrailSurrogateMax) {
        if (next <= unicode::TrailSurrogateMax) {
            trail_ranges->append(CharacterRange::Range(first, next));
            return;
        }
        trail_ranges->append(CharacterRange::Range(first, unicode::TrailSurrogateMax));
        first = unicode::TrailSurrogateMax + 1;
    }
    if (first <= unicode::UTF16Max) {
        if (next <= unicode::UTF16Max) {
            ranges->append(CharacterRange::Range(first, next));
            return;
        }
        ranges->append(CharacterRange::Range(first, unicode::UTF16Max));
        first = unicode::NonBMPMin;
    }
    MOZ_ASSERT(unicode::UTF16Max + 1 == unicode::NonBMPMin);
    wide_ranges->append(WideCharRange::Range(first, next));
}

// Negate a vector of ranges by subtracting its ranges from a range
// encompassing the full range of possible values.
template <typename RangeType>
static inline void
NegateUnicodeRanges(LifoAlloc* alloc, InfallibleVector<RangeType, 1>** ranges,
                    RangeType full_range)
{
    typedef InfallibleVector<RangeType, 1> RangeVector;
    RangeVector* tmp_ranges = alloc->newInfallible<RangeVector>(*alloc);
    tmp_ranges->append(full_range);
    RangeVector* result_ranges = alloc->newInfallible<RangeVector>(*alloc);

    // Perform the following calculation:
    //   result_ranges = tmp_ranges - ranges
    // with the following steps:
    //   result_ranges = tmp_ranges - ranges[0]
    //   SWAP(result_ranges, tmp_ranges)
    //   result_ranges = tmp_ranges - ranges[1]
    //   SWAP(result_ranges, tmp_ranges)
    //   ...
    //   result_ranges = tmp_ranges - ranges[N-1]
    //   SWAP(result_ranges, tmp_ranges)
    // The last SWAP is just for simplicity of the loop.
    for (size_t i = 0; i < (*ranges)->length(); i++) {
        result_ranges->clear();

        const RangeType& range = (**ranges)[i];
        for (size_t j = 0; j < tmp_ranges->length(); j++) {
            const RangeType& tmpRange = (*tmp_ranges)[j];
            auto from1 = tmpRange.from();
            auto to1 = tmpRange.to();
            auto from2 = range.from();
            auto to2 = range.to();

            if (from1 < from2) {
                if (to1 < from2) {
                    result_ranges->append(tmpRange);
                } else if (to1 <= to2) {
                    result_ranges->append(RangeType::Range(from1, from2 - 1));
                } else {
                    result_ranges->append(RangeType::Range(from1, from2 - 1));
                    result_ranges->append(RangeType::Range(to2 + 1, to1));
                }
            } else if (from1 <= to2) {
                if (to1 > to2)
                    result_ranges->append(RangeType::Range(to2 + 1, to1));
            } else {
                result_ranges->append(tmpRange);
            }
        }

        auto tmp = tmp_ranges;
        tmp_ranges = result_ranges;
        result_ranges = tmp;
    }

    // After the loop, result is pointed at by tmp_ranges, instead of
    // result_ranges.
    *ranges = tmp_ranges;
}

static bool
WideCharRangesContain(WideCharRangeVector* wide_ranges, widechar c)
{
    for (size_t i = 0; i < wide_ranges->length(); i++) {
        const WideCharRange& range = (*wide_ranges)[i];
        if (range.Contains(c))
            return true;
    }
    return false;
}

static void
CalculateCaseInsensitiveRanges(LifoAlloc* alloc, widechar from, widechar to, int32_t diff,
                               WideCharRangeVector* wide_ranges,
                               WideCharRangeVector** tmp_wide_ranges)
{
    widechar contains_from = 0;
    widechar contains_to = 0;
    for (widechar c = from; c <= to; c++) {
        if (WideCharRangesContain(wide_ranges, c) &&
            !WideCharRangesContain(wide_ranges, c + diff))
        {
            if (contains_from == 0)
                contains_from = c;
            contains_to = c;
        } else if (contains_from != 0) {
            if (!*tmp_wide_ranges)
                *tmp_wide_ranges = alloc->newInfallible<WideCharRangeVector>(*alloc);

            (*tmp_wide_ranges)->append(WideCharRange::Range(contains_from + diff,
                                                            contains_to + diff));
            contains_from = 0;
        }
    }

    if (contains_from != 0) {
        if (!*tmp_wide_ranges)
            *tmp_wide_ranges = alloc->newInfallible<WideCharRangeVector>(*alloc);

        (*tmp_wide_ranges)->append(WideCharRange::Range(contains_from + diff,
                                                        contains_to + diff));
    }
}

static RegExpTree*
UnicodeRangesAtom(LifoAlloc* alloc,
                  CharacterRangeVector* ranges,
                  CharacterRangeVector* lead_ranges,
                  CharacterRangeVector* trail_ranges,
                  WideCharRangeVector* wide_ranges,
                  bool is_negated,
                  bool ignore_case)
{
    // Calculate case folding for non-BMP first and negate the range if needed.
    if (ignore_case) {
        WideCharRangeVector* tmp_wide_ranges = nullptr;
#define CALL_CALC(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
        CalculateCaseInsensitiveRanges(alloc, FROM, TO, DIFF, wide_ranges, &tmp_wide_ranges);
        FOR_EACH_NON_BMP_CASE_FOLDING(CALL_CALC)
        FOR_EACH_NON_BMP_REV_CASE_FOLDING(CALL_CALC)
#undef CALL_CALC

        if (tmp_wide_ranges) {
            for (size_t i = 0; i < tmp_wide_ranges->length(); i++)
                wide_ranges->append((*tmp_wide_ranges)[i]);
        }
    }

    if (is_negated) {
        NegateUnicodeRanges(alloc, &lead_ranges, LeadSurrogateRange());
        NegateUnicodeRanges(alloc, &trail_ranges, TrailSurrogateRange());
        NegateUnicodeRanges(alloc, &wide_ranges, NonBMPRange());
    }

    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);

    bool added = false;

    if (is_negated) {
        ranges->append(LeadSurrogateRange());
        ranges->append(TrailSurrogateRange());
    }
    if (ranges->length() > 0) {
        builder->AddAtom(alloc->newInfallible<RegExpCharacterClass>(ranges, is_negated));
        added = true;
    }

    if (lead_ranges->length() > 0) {
        if (added)
            builder->NewAlternative();
        builder->AddAtom(alloc->newInfallible<RegExpCharacterClass>(lead_ranges, false));
        builder->AddAtom(NegativeLookahead(alloc, unicode::TrailSurrogateMin,
                                           unicode::TrailSurrogateMax));
        added = true;
    }

    if (trail_ranges->length() > 0) {
        if (added)
            builder->NewAlternative();
        builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(
            RegExpAssertion::NOT_AFTER_LEAD_SURROGATE));
        builder->AddAtom(alloc->newInfallible<RegExpCharacterClass>(trail_ranges, false));
        added = true;
    }

    for (size_t i = 0; i < wide_ranges->length(); i++) {
        if (added)
            builder->NewAlternative();

        const WideCharRange& range = (*wide_ranges)[i];
        widechar from = range.from();
        widechar to = range.to();
        char16_t from_lead, from_trail;
        char16_t to_lead, to_trail;

        unicode::UTF16Encode(from, &from_lead, &from_trail);
        if (from == to) {
            builder->AddCharacter(from_lead);
            builder->AddCharacter(from_trail);
        } else {
            unicode::UTF16Encode(to, &to_lead, &to_trail);
            if (from_lead == to_lead) {
                MOZ_ASSERT(from_trail != to_trail);
                builder->AddCharacter(from_lead);
                builder->AddAtom(RangeAtom(alloc, from_trail, to_trail));
            } else if (from_trail == unicode::TrailSurrogateMin &&
                       to_trail == unicode::TrailSurrogateMax)
            {
                builder->AddAtom(RangeAtom(alloc, from_lead, to_lead));
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin,
                                           unicode::TrailSurrogateMax));
            } else if (from_lead + 1 == to_lead) {
                builder->AddCharacter(from_lead);
                builder->AddAtom(RangeAtom(alloc, from_trail, unicode::TrailSurrogateMax));

                builder->NewAlternative();

                builder->AddCharacter(to_lead);
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin, to_trail));
            } else if (from_lead + 2 == to_lead) {
                builder->AddCharacter(from_lead);
                builder->AddAtom(RangeAtom(alloc, from_trail, unicode::TrailSurrogateMax));

                builder->NewAlternative();

                builder->AddCharacter(from_lead + 1);
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin,
                                           unicode::TrailSurrogateMax));

                builder->NewAlternative();

                builder->AddCharacter(to_lead);
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin, to_trail));
            } else {
                builder->AddCharacter(from_lead);
                builder->AddAtom(RangeAtom(alloc, from_trail, unicode::TrailSurrogateMax));

                builder->NewAlternative();

                builder->AddAtom(RangeAtom(alloc, from_lead + 1, to_lead - 1));
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin,
                                           unicode::TrailSurrogateMax));

                builder->NewAlternative();

                builder->AddCharacter(to_lead);
                builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin, to_trail));
            }
        }
        added = true;
    }

    return builder->ToRegExp();
}

template <typename CharT>
RegExpTree*
RegExpParser<CharT>::ParseCharacterClass()
{
    MOZ_ASSERT(current() == '[');
    Advance();
    bool is_negated = false;
    if (current() == '^') {
        is_negated = true;
        Advance();
    }
    CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    CharacterRangeVector* lead_ranges = nullptr;
    CharacterRangeVector* trail_ranges = nullptr;
    WideCharRangeVector* wide_ranges = nullptr;

    if (unicode_) {
        lead_ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
        trail_ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
        wide_ranges = alloc->newInfallible<WideCharRangeVector>(*alloc);
    }

    while (has_more() && current() != ']') {
        char16_t char_class = kNoCharClass;
        widechar first = 0;
        if (!ParseClassAtom(&char_class, &first))
            return nullptr;
        if (current() == '-') {
            Advance();
            if (current() == kEndMarker) {
                // If we reach the end we break out of the loop and let the
                // following code report an error.
                break;
            } else if (current() == ']') {
                if (unicode_) {
                    AddCharOrEscapeUnicode(alloc, ranges, lead_ranges, trail_ranges, wide_ranges,
                                           char_class, first, ignore_case_);
                } else {
                    AddCharOrEscape(alloc, ranges, char_class, first);
                }
                ranges->append(CharacterRange::Singleton('-'));
                break;
            }
            char16_t char_class_2 = kNoCharClass;
            widechar next = 0;
            if (!ParseClassAtom(&char_class_2, &next))
                return nullptr;
            if (char_class != kNoCharClass || char_class_2 != kNoCharClass) {
                if (unicode_)
                    return ReportError(JSMSG_RANGE_WITH_CLASS_ESCAPE);

                // Either end is an escaped character class. Treat the '-' verbatim.
                AddCharOrEscape(alloc, ranges, char_class, first);
                ranges->append(CharacterRange::Singleton('-'));
                AddCharOrEscape(alloc, ranges, char_class_2, next);
                continue;
            }
            if (first > next)
                return ReportError(JSMSG_BAD_CLASS_RANGE);
            if (unicode_)
                AddUnicodeRange(alloc, ranges, lead_ranges, trail_ranges,wide_ranges, first, next);
            else
                ranges->append(CharacterRange::Range(first, next));
        } else {
            if (unicode_) {
                AddCharOrEscapeUnicode(alloc, ranges, lead_ranges, trail_ranges, wide_ranges,
                                       char_class, first, ignore_case_);
            } else {
                AddCharOrEscape(alloc, ranges, char_class, first);
            }
        }
    }
    if (!has_more())
        return ReportError(JSMSG_UNTERM_CLASS);
    Advance();
    if (!unicode_) {
        if (ranges->length() == 0) {
            ranges->append(CharacterRange::Everything());
            is_negated = !is_negated;
        }
        return alloc->newInfallible<RegExpCharacterClass>(ranges, is_negated);
    }

    if (!is_negated && ranges->length() == 0 && lead_ranges->length() == 0 &&
        trail_ranges->length() == 0 && wide_ranges->length() == 0)
    {
        ranges->append(CharacterRange::Everything());
        return alloc->newInfallible<RegExpCharacterClass>(ranges, true);
    }

    return UnicodeRangesAtom(alloc, ranges, lead_ranges, trail_ranges, wide_ranges, is_negated,
                             ignore_case_);
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseClassAtom(char16_t* char_class, widechar* value)
{
    MOZ_ASSERT(*char_class == kNoCharClass);
    widechar first = current();
    if (first == '\\') {
        switch (Next()) {
          case 'w': case 'W': case 'd': case 'D': case 's': case 'S': {
            *char_class = Next();
            Advance(2);
            return true;
          }
          case kEndMarker:
            return ReportError(JSMSG_ESCAPE_AT_END_OF_REGEXP);
          default:
            if (!ParseClassCharacterEscape(value))
                return false;
            return true;
        }
    } else {
        if (unicode_) {
            char16_t lead, trail;
            if (ParseRawSurrogatePair(&lead, &trail)) {
                *value = unicode::UTF16Decode(lead, trail);
                return true;
            }
        }
        Advance();
        *value = first;
        return true;
    }
}

// In order to know whether an escape is a backreference or not we have to scan
// the entire regexp and find the number of capturing parentheses.  However we
// don't want to scan the regexp twice unless it is necessary.  This mini-parser
// is called when needed.  It can see the difference between capturing and
// noncapturing parentheses and can skip character classes and backslash-escaped
// characters.
template <typename CharT>
void
RegExpParser<CharT>::ScanForCaptures()
{
    // Start with captures started previous to current position
    int capture_count = captures_started();
    // Add count of captures after this position.
    widechar n;
    while ((n = current()) != kEndMarker) {
        Advance();
        switch (n) {
          case '\\':
            Advance();
            break;
          case '[': {
            widechar c;
            while ((c = current()) != kEndMarker) {
                Advance();
                if (c == '\\') {
                    Advance();
                } else {
                    if (c == ']') break;
                }
            }
            break;
          }
          case '(':
            if (current() != '?') capture_count++;
            break;
        }
    }
    capture_count_ = capture_count;
    is_scanned_for_captures_ = true;
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseBackReferenceIndex(int* index_out)
{
    MOZ_ASSERT('\\' == current());
    MOZ_ASSERT('1' <= Next() && Next() <= '9');

    // Try to parse a decimal literal that is no greater than the total number
    // of left capturing parentheses in the input.
    const CharT* start = position();
    int value = Next() - '0';
    Advance(2);
    while (true) {
        widechar c = current();
        if (IsDecimalDigit(c)) {
            value = 10 * value + (c - '0');
            if (value > kMaxCaptures) {
                Reset(start);
                return false;
            }
            Advance();
        } else {
            break;
        }
    }
    if (value > captures_started()) {
        if (!is_scanned_for_captures_) {
            const CharT* saved_position = position();
            ScanForCaptures();
            Reset(saved_position);
        }
        if (value > capture_count_) {
            Reset(start);
            return false;
        }
    }
    *index_out = value;
    return true;
}

// QuantifierPrefix ::
//   { DecimalDigits }
//   { DecimalDigits , }
//   { DecimalDigits , DecimalDigits }
//
// Returns true if parsing succeeds, and set the min_out and max_out
// values. Values are truncated to RegExpTree::kInfinity if they overflow.
template <typename CharT>
bool
RegExpParser<CharT>::ParseIntervalQuantifier(int* min_out, int* max_out)
{
    MOZ_ASSERT(current() == '{');
    const CharT* start = position();
    Advance();
    int min = 0;
    if (!IsDecimalDigit(current())) {
        Reset(start);
        return false;
    }
    while (IsDecimalDigit(current())) {
        int next = current() - '0';
        if (min > (RegExpTree::kInfinity - next) / 10) {
            // Overflow. Skip past remaining decimal digits and return -1.
            do {
                Advance();
            } while (IsDecimalDigit(current()));
            min = RegExpTree::kInfinity;
            break;
        }
        min = 10 * min + next;
        Advance();
    }
    int max = 0;
    if (current() == '}') {
        max = min;
        Advance();
    } else if (current() == ',') {
        Advance();
        if (current() == '}') {
            max = RegExpTree::kInfinity;
            Advance();
        } else {
            while (IsDecimalDigit(current())) {
                int next = current() - '0';
                if (max > (RegExpTree::kInfinity - next) / 10) {
                    do {
                        Advance();
                    } while (IsDecimalDigit(current()));
                    max = RegExpTree::kInfinity;
                    break;
                }
                max = 10 * max + next;
                Advance();
            }
            if (current() != '}') {
                Reset(start);
                return false;
            }
            Advance();
        }
    } else {
        Reset(start);
        return false;
    }
    *min_out = min;
    *max_out = max;
    return true;
}

// Pattern ::
//   Disjunction
template <typename CharT>
RegExpTree*
RegExpParser<CharT>::ParsePattern()
{
    RegExpTree* result = ParseDisjunction();
    MOZ_ASSERT_IF(result, !has_more());
    return result;
}

static inline RegExpTree*
CaseFoldingSurrogatePairAtom(LifoAlloc* alloc, char16_t lead, char16_t trail, int32_t diff)
{
    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);

    builder->AddCharacter(lead);
    CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    ranges->append(CharacterRange::Range(trail, trail));
    ranges->append(CharacterRange::Range(trail + diff, trail + diff));
    builder->AddAtom(alloc->newInfallible<RegExpCharacterClass>(ranges, false));

    return builder->ToRegExp();
}

static inline RegExpTree*
SurrogatePairAtom(LifoAlloc* alloc, char16_t lead, char16_t trail, bool ignore_case)
{
    if (ignore_case) {
#define CALL_ATOM(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF) \
        if (lead == LEAD &&trail >= TRAIL_FROM && trail <= TRAIL_TO) \
            return CaseFoldingSurrogatePairAtom(alloc, lead, trail, DIFF);
        FOR_EACH_NON_BMP_CASE_FOLDING(CALL_ATOM)
        FOR_EACH_NON_BMP_REV_CASE_FOLDING(CALL_ATOM)
#undef CALL_ATOM
    }

    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);
    builder->AddCharacter(lead);
    builder->AddCharacter(trail);
    return builder->ToRegExp();
}

static inline RegExpTree*
LeadSurrogateAtom(LifoAlloc* alloc, char16_t value)
{
    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);
    builder->AddCharacter(value);
    builder->AddAtom(NegativeLookahead(alloc, unicode::TrailSurrogateMin,
                                       unicode::TrailSurrogateMax));
    return builder->ToRegExp();
}

static inline RegExpTree*
TrailSurrogateAtom(LifoAlloc* alloc, char16_t value)
{
    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);
    builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(
        RegExpAssertion::NOT_AFTER_LEAD_SURROGATE));
    builder->AddCharacter(value);
    return builder->ToRegExp();
}

static inline RegExpTree*
UnicodeEverythingAtom(LifoAlloc* alloc)
{
    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);

    // everything except \x0a, \x0d, \u2028 and \u2029

    CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    AddClassNegated(kLineTerminatorAndSurrogateRanges,
                    kLineTerminatorAndSurrogateRangeCount,
                    ranges);
    builder->AddAtom(alloc->newInfallible<RegExpCharacterClass>(ranges, false));

    builder->NewAlternative();

    builder->AddAtom(RangeAtom(alloc, unicode::LeadSurrogateMin, unicode::LeadSurrogateMax));
    builder->AddAtom(NegativeLookahead(alloc, unicode::TrailSurrogateMin,
                                       unicode::TrailSurrogateMax));

    builder->NewAlternative();

    builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(
        RegExpAssertion::NOT_AFTER_LEAD_SURROGATE));
    builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin, unicode::TrailSurrogateMax));

    builder->NewAlternative();

    builder->AddAtom(RangeAtom(alloc, unicode::LeadSurrogateMin, unicode::LeadSurrogateMax));
    builder->AddAtom(RangeAtom(alloc, unicode::TrailSurrogateMin, unicode::TrailSurrogateMax));

    return builder->ToRegExp();
}

RegExpTree*
UnicodeCharacterClassEscapeAtom(LifoAlloc* alloc, char16_t char_class, bool ignore_case)
{
    CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    CharacterRangeVector* lead_ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    CharacterRangeVector* trail_ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
    WideCharRangeVector* wide_ranges = alloc->newInfallible<WideCharRangeVector>(*alloc);
    AddCharOrEscapeUnicode(alloc, ranges, lead_ranges, trail_ranges, wide_ranges, char_class, 0,
                           ignore_case);

    return UnicodeRangesAtom(alloc, ranges, lead_ranges, trail_ranges, wide_ranges, false, false);
}

static inline RegExpTree*
UnicodeBackReferenceAtom(LifoAlloc* alloc, RegExpTree* atom)
{
    // If a back reference has a standalone lead surrogate as its last
    // character, then that lead surrogate shouldn't match lead surrogates that
    // are paired with a corresponding trail surrogate.
    RegExpBuilder* builder = alloc->newInfallible<RegExpBuilder>(alloc);

    builder->AddAtom(atom);
    builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(
        RegExpAssertion::NOT_IN_SURROGATE_PAIR));

    return builder->ToRegExp();
}

// Disjunction ::
//   Alternative
//   Alternative | Disjunction
// Alternative ::
//   [empty]
//   Term Alternative
// Term ::
//   Assertion
//   Atom
//   Atom Quantifier
template <typename CharT>
RegExpTree*
RegExpParser<CharT>::ParseDisjunction()
{
    // Used to store current state while parsing subexpressions.
    RegExpParserState initial_state(alloc, nullptr, INITIAL, 0);
    RegExpParserState* stored_state = &initial_state;
    // Cache the builder in a local variable for quick access.
    RegExpBuilder* builder = initial_state.builder();
    while (true) {
        switch (current()) {
          case kEndMarker:
            if (stored_state->IsSubexpression()) {
                // Inside a parenthesized group when hitting end of input.
                return ReportError(JSMSG_MISSING_PAREN);
            }
            MOZ_ASSERT(INITIAL == stored_state->group_type());
            // Parsing completed successfully.
            return builder->ToRegExp();
          case ')': {
            if (!stored_state->IsSubexpression())
                return ReportError(JSMSG_UNMATCHED_RIGHT_PAREN);
            MOZ_ASSERT(INITIAL != stored_state->group_type());

            Advance();
            // End disjunction parsing and convert builder content to new single
            // regexp atom.
            RegExpTree* body = builder->ToRegExp();

            int end_capture_index = captures_started();

            int capture_index = stored_state->capture_index();
            SubexpressionType group_type = stored_state->group_type();

            // Restore previous state.
            stored_state = stored_state->previous_state();
            builder = stored_state->builder();

            // Build result of subexpression.
            if (group_type == CAPTURE) {
                RegExpCapture* capture = alloc->newInfallible<RegExpCapture>(body, capture_index);
                (*captures_)[capture_index - 1] = capture;
                body = capture;
            } else if (group_type != GROUPING) {
                MOZ_ASSERT(group_type == POSITIVE_LOOKAHEAD ||
                           group_type == NEGATIVE_LOOKAHEAD);
                bool is_positive = (group_type == POSITIVE_LOOKAHEAD);
                body = alloc->newInfallible<RegExpLookahead>(body,
                                                   is_positive,
                                                   end_capture_index - capture_index,
                                                   capture_index);
            }
            builder->AddAtom(body);
            if (unicode_ && (group_type == POSITIVE_LOOKAHEAD || group_type == NEGATIVE_LOOKAHEAD))
                continue;
            // For compatability with JSC and ES3, we allow quantifiers after
            // lookaheads, and break in all cases.
            break;
          }
          case '|': {
            Advance();
            builder->NewAlternative();
            continue;
          }
          case '*':
          case '+':
          case '?':
            return ReportError(JSMSG_NOTHING_TO_REPEAT);
          case '^': {
            Advance();
            if (multiline_) {
                builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(RegExpAssertion::START_OF_LINE));
            } else {
                builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(RegExpAssertion::START_OF_INPUT));
                set_contains_anchor();
            }
            continue;
          }
          case '$': {
            Advance();
            RegExpAssertion::AssertionType assertion_type =
                multiline_ ? RegExpAssertion::END_OF_LINE :
                RegExpAssertion::END_OF_INPUT;
            builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(assertion_type));
            continue;
          }
          case '.': {
            Advance();
            // everything except \x0a, \x0d, \u2028 and \u2029
            if (unicode_) {
                builder->AddAtom(UnicodeEverythingAtom(alloc));
                break;
            }
            CharacterRangeVector* ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
            CharacterRange::AddClassEscape(alloc, '.', ranges);
            RegExpTree* atom = alloc->newInfallible<RegExpCharacterClass>(ranges, false);
            builder->AddAtom(atom);
            break;
          }
          case '(': {
            SubexpressionType subexpr_type = CAPTURE;
            Advance();
            if (current() == '?') {
                switch (Next()) {
                  case ':':
                    subexpr_type = GROUPING;
                    break;
                  case '=':
                    subexpr_type = POSITIVE_LOOKAHEAD;
                    break;
                  case '!':
                    subexpr_type = NEGATIVE_LOOKAHEAD;
                    break;
                  default:
                    return ReportError(JSMSG_INVALID_GROUP);
                }
                Advance(2);
            } else {
                if (captures_ == nullptr)
                    captures_ = alloc->newInfallible<RegExpCaptureVector>(*alloc);
                if (captures_started() >= kMaxCaptures)
                    return ReportError(JSMSG_TOO_MANY_PARENS);
                captures_->append((RegExpCapture*) nullptr);
            }
            // Store current state and begin new disjunction parsing.
            stored_state = alloc->newInfallible<RegExpParserState>(alloc, stored_state, subexpr_type,
                                                                   captures_started());
            builder = stored_state->builder();
            continue;
          }
          case '[': {
            RegExpTree* atom = ParseCharacterClass();
            if (!atom)
                return nullptr;
            builder->AddAtom(atom);
            break;
          }
            // Atom ::
            //   \ AtomEscape
          case '\\':
            switch (Next()) {
              case kEndMarker:
                return ReportError(JSMSG_ESCAPE_AT_END_OF_REGEXP);
              case 'b':
                Advance(2);
                builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(RegExpAssertion::BOUNDARY));
                continue;
              case 'B':
                Advance(2);
                builder->AddAssertion(alloc->newInfallible<RegExpAssertion>(RegExpAssertion::NON_BOUNDARY));
                continue;
                // AtomEscape ::
                //   CharacterClassEscape
                //
                // CharacterClassEscape :: one of
                //   d D s S w W
              case 'D': case 'S': case 'W':
                if (unicode_) {
                    Advance();
                    builder->AddAtom(UnicodeCharacterClassEscapeAtom(alloc, current(),
                                                                     ignore_case_));
                    Advance();
                    break;
                }
                MOZ_FALLTHROUGH;
              case 'd': case 's': case 'w': {
                widechar c = Next();
                Advance(2);
                CharacterRangeVector* ranges =
                    alloc->newInfallible<CharacterRangeVector>(*alloc);
                if (unicode_)
                    CharacterRange::AddClassEscapeUnicode(alloc, c, ranges, ignore_case_);
                else
                    CharacterRange::AddClassEscape(alloc, c, ranges);
                RegExpTree* atom = alloc->newInfallible<RegExpCharacterClass>(ranges, false);
                builder->AddAtom(atom);
                break;
              }
              case '1': case '2': case '3': case '4': case '5': case '6':
              case '7': case '8': case '9': {
                int index = 0;
                if (ParseBackReferenceIndex(&index)) {
                    RegExpCapture* capture = nullptr;
                    if (captures_ != nullptr && index <= (int) captures_->length()) {
                        capture = (*captures_)[index - 1];
                    }
                    if (capture == nullptr) {
                        builder->AddEmpty();
                        break;
                    }
                    RegExpTree* atom = alloc->newInfallible<RegExpBackReference>(capture);
                    if (unicode_)
                        builder->AddAtom(UnicodeBackReferenceAtom(alloc, atom));
                    else
                        builder->AddAtom(atom);
                    break;
                }
                if (unicode_)
                    return ReportError(JSMSG_BACK_REF_OUT_OF_RANGE);
                widechar first_digit = Next();
                if (first_digit == '8' || first_digit == '9') {
                    // Treat as identity escape
                    builder->AddCharacter(first_digit);
                    Advance(2);
                    break;
                }
                MOZ_FALLTHROUGH;
              }
              case '0': {
                if (unicode_) {
                    Advance(2);
                    if (IsDecimalDigit(current()))
                        return ReportError(JSMSG_INVALID_DECIMAL_ESCAPE);
                    builder->AddCharacter(0);
                    break;
                }

                Advance();
                widechar octal = ParseOctalLiteral();
                builder->AddCharacter(octal);
                break;
              }
                // ControlEscape :: one of
                //   f n r t v
              case 'f':
                Advance(2);
                builder->AddCharacter('\f');
                break;
              case 'n':
                Advance(2);
                builder->AddCharacter('\n');
                break;
              case 'r':
                Advance(2);
                builder->AddCharacter('\r');
                break;
              case 't':
                Advance(2);
                builder->AddCharacter('\t');
                break;
              case 'v':
                Advance(2);
                builder->AddCharacter('\v');
                break;
              case 'c': {
                Advance();
                widechar controlLetter = Next();
                // Special case if it is an ASCII letter.
                // Convert lower case letters to uppercase.
                widechar letter = controlLetter & ~('a' ^ 'A');
                if (letter < 'A' || 'Z' < letter) {
                    if (unicode_)
                        return ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
                    // controlLetter is not in range 'A'-'Z' or 'a'-'z'.
                    // This is outside the specification. We match JSC in
                    // reading the backslash as a literal character instead
                    // of as starting an escape.
                    builder->AddCharacter('\\');
                } else {
                    Advance(2);
                    builder->AddCharacter(controlLetter & 0x1f);
                }
                break;
              }
              case 'x': {
                Advance(2);
                widechar value;
                if (ParseHexEscape(2, &value)) {
                    builder->AddCharacter(value);
                } else {
                    if (unicode_)
                        return ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
                    builder->AddCharacter('x');
                }
                break;
              }
              case 'u': {
                Advance(2);
                widechar value;
                if (unicode_) {
                    if (current() == '{') {
                        if (!ParseBracedHexEscape(&value))
                            return nullptr;
                        if (unicode::IsLeadSurrogate(value)) {
                            builder->AddAtom(LeadSurrogateAtom(alloc, value));
                        } else if (unicode::IsTrailSurrogate(value)) {
                            builder->AddAtom(TrailSurrogateAtom(alloc, value));
                        } else if (value >= unicode::NonBMPMin) {
                            char16_t lead, trail;
                            unicode::UTF16Encode(value, &lead, &trail);
                            builder->AddAtom(SurrogatePairAtom(alloc, lead, trail,
                                                               ignore_case_));
                        } else {
                            builder->AddCharacter(value);
                        }
                    } else if (ParseHexEscape(4, &value)) {
                        if (unicode::IsLeadSurrogate(value)) {
                            widechar trail;
                            if (ParseTrailSurrogate(&trail)) {
                                builder->AddAtom(SurrogatePairAtom(alloc, value, trail,
                                                                   ignore_case_));
                            } else {
                                builder->AddAtom(LeadSurrogateAtom(alloc, value));
                            }
                        } else if (unicode::IsTrailSurrogate(value)) {
                            builder->AddAtom(TrailSurrogateAtom(alloc, value));
                        } else {
                            builder->AddCharacter(value);
                        }
                    } else {
                        return ReportError(JSMSG_INVALID_UNICODE_ESCAPE);
                    }
                    break;
                }
                if (ParseHexEscape(4, &value)) {
                    builder->AddCharacter(value);
                } else {
                    builder->AddCharacter('u');
                }
                break;
              }
              default:
                // Identity escape.
                if (unicode_ && !IsSyntaxCharacter(Next()))
                    return ReportError(JSMSG_INVALID_IDENTITY_ESCAPE);
                builder->AddCharacter(Next());
                Advance(2);
                break;
            }
            break;
          case '{': {
            if (unicode_)
                return ReportError(JSMSG_RAW_BRACE_IN_REGEP);
            int dummy;
            if (ParseIntervalQuantifier(&dummy, &dummy))
                return ReportError(JSMSG_NOTHING_TO_REPEAT);
            MOZ_FALLTHROUGH;
          }
          default:
            if (unicode_) {
                char16_t lead, trail;
                if (ParseRawSurrogatePair(&lead, &trail)) {
                    builder->AddAtom(SurrogatePairAtom(alloc, lead, trail, ignore_case_));
                } else {
                    widechar c = current();
                    if (unicode::IsLeadSurrogate(c))
                        builder->AddAtom(LeadSurrogateAtom(alloc, c));
                    else if (unicode::IsTrailSurrogate(c))
                        builder->AddAtom(TrailSurrogateAtom(alloc, c));
                    else if (c == ']')
                        return ReportError(JSMSG_RAW_BRACKET_IN_REGEP);
                    else if (c == '}')
                        return ReportError(JSMSG_RAW_BRACE_IN_REGEP);
                    else
                        builder->AddCharacter(c);
                    Advance();
                }
                break;
            }
            builder->AddCharacter(current());
            Advance();
            break;
        }  // end switch(current())

        int min;
        int max;
        switch (current()) {
            // QuantifierPrefix ::
            //   *
            //   +
            //   ?
            //   {
          case '*':
            min = 0;
            max = RegExpTree::kInfinity;
            Advance();
            break;
          case '+':
            min = 1;
            max = RegExpTree::kInfinity;
            Advance();
            break;
          case '?':
            min = 0;
            max = 1;
            Advance();
            break;
          case '{':
            if (ParseIntervalQuantifier(&min, &max)) {
                if (max < min)
                    return ReportError(JSMSG_NUMBERS_OUT_OF_ORDER);
                break;
            } else {
                continue;
            }
          default:
            continue;
        }
        RegExpQuantifier::QuantifierType quantifier_type = RegExpQuantifier::GREEDY;
        if (current() == '?') {
            quantifier_type = RegExpQuantifier::NON_GREEDY;
            Advance();
        }
        builder->AddQuantifierToAtom(min, max, quantifier_type);
    }
}

template class irregexp::RegExpParser<Latin1Char>;
template class irregexp::RegExpParser<char16_t>;

template <typename CharT>
static bool
ParsePattern(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc,
             const CharT* chars, size_t length,
             bool multiline, bool match_only, bool unicode, bool ignore_case,
             bool global, bool sticky, RegExpCompileData* data)
{
    // We shouldn't strip pattern for exec, or test with global/sticky,
    // to reflect correct match position and lastIndex.
    if (match_only && !global && !sticky) {
        // Try to strip a leading '.*' from the RegExp, but only if it is not
        // followed by a '?' (which will affect how the .* is parsed). This
        // pattern will affect the captures produced by the RegExp, but not
        // whether there is a match or not.
        if (length >= 3 && chars[0] == '.' && chars[1] == '*' && chars[2] != '?') {
            chars += 2;
            length -= 2;
        }

        // Try to strip a trailing '.*' from the RegExp, which as above will
        // affect the captures but not whether there is a match. Only do this
        // when there are no other meta characters in the RegExp, so that we
        // are sure this will not affect how the RegExp is parsed.
        if (length >= 3 && !HasRegExpMetaChars(chars, length - 2) &&
            chars[length - 2] == '.' && chars[length - 1] == '*')
        {
            length -= 2;
        }
    }

    RegExpParser<CharT> parser(ts, &alloc, chars, chars + length, multiline, unicode, ignore_case);
    data->tree = parser.ParsePattern();
    if (!data->tree)
        return false;

    data->simple = parser.simple();
    data->contains_anchor = parser.contains_anchor();
    data->capture_count = parser.captures_started();
    return true;
}

bool
irregexp::ParsePattern(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc, JSAtom* str,
                       bool multiline, bool match_only, bool unicode, bool ignore_case,
                       bool global, bool sticky, RegExpCompileData* data)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::ParsePattern(ts, alloc, str->latin1Chars(nogc), str->length(),
                            multiline, match_only, unicode, ignore_case, global, sticky, data)
           : ::ParsePattern(ts, alloc, str->twoByteChars(nogc), str->length(),
                            multiline, match_only, unicode, ignore_case, global, sticky, data);
}

template <typename CharT>
static bool
ParsePatternSyntax(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc,
                   const CharT* chars, size_t length, bool unicode)
{
    LifoAllocScope scope(&alloc);

    RegExpParser<CharT> parser(ts, &alloc, chars, chars + length, false, unicode, false);
    return parser.ParsePattern() != nullptr;
}

bool
irregexp::ParsePatternSyntax(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc, JSAtom* str,
                             bool unicode)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::ParsePatternSyntax(ts, alloc, str->latin1Chars(nogc), str->length(), unicode)
           : ::ParsePatternSyntax(ts, alloc, str->twoByteChars(nogc), str->length(), unicode);
}

bool
irregexp::ParsePatternSyntax(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc,
                             const mozilla::Range<const char16_t> chars, bool unicode)
{
    return ::ParsePatternSyntax(ts, alloc, chars.begin().get(), chars.length(), unicode);
}
