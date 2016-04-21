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

#include "frontend/TokenStream.h"

using namespace js;
using namespace js::irregexp;

// ----------------------------------------------------------------------------
// RegExpBuilder

RegExpBuilder::RegExpBuilder(LifoAlloc* alloc)
  : alloc(alloc),
    pending_empty_(false),
    characters_(nullptr),
    last_added_(ADD_NONE)
{}

void
RegExpBuilder::FlushCharacters()
{
    pending_empty_ = false;
    if (characters_ != nullptr) {
        RegExpTree* atom = alloc->newInfallible<RegExpAtom>(characters_);
        characters_ = nullptr;
        text_.Add(alloc, atom);
        last_added_ = ADD_ATOM;
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
    last_added_ = ADD_CHAR;
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
    last_added_ = ADD_ATOM;
}

void
RegExpBuilder::AddAssertion(RegExpTree* assert)
{
    FlushText();
    terms_.Add(alloc, assert);
    last_added_ = ADD_ASSERT;
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
    last_added_ = ADD_NONE;
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
            last_added_ = ADD_TERM;
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
    last_added_ = ADD_TERM;
}

// ----------------------------------------------------------------------------
// RegExpParser

template <typename CharT>
RegExpParser<CharT>::RegExpParser(frontend::TokenStream& ts, LifoAlloc* alloc,
                                  const CharT* chars, const CharT* end, bool multiline_mode)
  : ts(ts),
    alloc(alloc),
    captures_(nullptr),
    next_pos_(chars),
    end_(end),
    current_(kEndMarker),
    capture_count_(0),
    has_more_(true),
    multiline_(multiline_mode),
    simple_(false),
    contains_anchor_(false),
    is_scanned_for_captures_(false)
{
    Advance();
}

template <typename CharT>
RegExpTree*
RegExpParser<CharT>::ReportError(unsigned errorNumber)
{
    gc::AutoSuppressGC suppressGC(ts.context());
    ts.reportError(errorNumber);
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
size_t
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
RegExpParser<CharT>::ParseHexEscape(int length, size_t* value)
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
widechar
RegExpParser<CharT>::ParseClassCharacterEscape()
{
    MOZ_ASSERT(current() == '\\');
    MOZ_ASSERT(has_next() && !IsSpecialClassEscape(Next()));
    Advance();
    switch (current()) {
      case 'b':
        Advance();
        return '\b';
      // ControlEscape :: one of
      //   f n r t v
      case 'f':
        Advance();
        return '\f';
      case 'n':
        Advance();
        return '\n';
      case 'r':
        Advance();
        return '\r';
      case 't':
        Advance();
        return '\t';
      case 'v':
        Advance();
        return '\v';
      case 'c': {
        widechar controlLetter = Next();
        widechar letter = controlLetter & ~('A' ^ 'a');
        // For compatibility with JSC, inside a character class
        // we also accept digits and underscore as control characters.
        if ((controlLetter >= '0' && controlLetter <= '9') ||
            controlLetter == '_' ||
            (letter >= 'A' && letter <= 'Z')) {
            Advance(2);
            // Control letters mapped to ASCII control characters in the range
            // 0x00-0x1f.
            return controlLetter & 0x1f;
        }
        // We match JSC in reading the backslash as a literal
        // character instead of as starting an escape.
        return '\\';
      }
      case '0': case '1': case '2': case '3': case '4': case '5':
      case '6': case '7':
        // For compatibility, we interpret a decimal escape that isn't
        // a back reference (and therefore either \0 or not valid according
        // to the specification) as a 1..3 digit octal character code.
        return ParseOctalLiteral();
      case 'x': {
        Advance();
        size_t value;
        if (ParseHexEscape(2, &value))
            return value;
        // If \x is not followed by a two-digit hexadecimal, treat it
        // as an identity escape.
        return 'x';
      }
      case 'u': {
        Advance();
        size_t value;
        if (ParseHexEscape(4, &value))
            return value;
        // If \u is not followed by a four-digit hexadecimal, treat it
        // as an identity escape.
        return 'u';
      }
      default: {
        // Extended identity escape. We accept any character that hasn't
        // been matched by a more specific case, not just the subset required
        // by the ECMAScript specification.
        widechar result = current();
        Advance();
        return result;
      }
    }
    return 0;
}

static const char16_t kNoCharClass = 0;

// Adds range or pre-defined character class to character ranges.
// If char_class is not kInvalidClass, it's interpreted as a class
// escape (i.e., 's' means whitespace, from '\s').
static inline void
AddRangeOrEscape(LifoAlloc* alloc,
                 CharacterRangeVector* ranges,
                 char16_t char_class,
                 CharacterRange range)
{
    if (char_class != kNoCharClass)
        CharacterRange::AddClassEscape(alloc, char_class, ranges);
    else
        ranges->append(range);
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
    while (has_more() && current() != ']') {
        char16_t char_class = kNoCharClass;
        CharacterRange first;
        if (!ParseClassAtom(&char_class, &first))
            return nullptr;
        if (current() == '-') {
            Advance();
            if (current() == kEndMarker) {
                // If we reach the end we break out of the loop and let the
                // following code report an error.
                break;
            } else if (current() == ']') {
                AddRangeOrEscape(alloc, ranges, char_class, first);
                ranges->append(CharacterRange::Singleton('-'));
                break;
            }
            char16_t char_class_2 = kNoCharClass;
            CharacterRange next;
            if (!ParseClassAtom(&char_class_2, &next))
                return nullptr;
            if (char_class != kNoCharClass || char_class_2 != kNoCharClass) {
                // Either end is an escaped character class. Treat the '-' verbatim.
                AddRangeOrEscape(alloc, ranges, char_class, first);
                ranges->append(CharacterRange::Singleton('-'));
                AddRangeOrEscape(alloc, ranges, char_class_2, next);
                continue;
            }
            if (first.from() > next.to())
                return ReportError(JSMSG_BAD_CLASS_RANGE);
            ranges->append(CharacterRange::Range(first.from(), next.to()));
        } else {
            AddRangeOrEscape(alloc, ranges, char_class, first);
        }
    }
    if (!has_more())
        return ReportError(JSMSG_UNTERM_CLASS);
    Advance();
    if (ranges->length() == 0) {
        ranges->append(CharacterRange::Everything());
        is_negated = !is_negated;
    }
    return alloc->newInfallible<RegExpCharacterClass>(ranges, is_negated);
}

template <typename CharT>
bool
RegExpParser<CharT>::ParseClassAtom(char16_t* char_class, CharacterRange* char_range)
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
            widechar c = ParseClassCharacterEscape();
            *char_range = CharacterRange::Singleton(c);
            return true;
        }
    } else {
        Advance();
        *char_range = CharacterRange::Singleton(first);
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
              case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
                widechar c = Next();
                Advance(2);
                CharacterRangeVector* ranges =
                    alloc->newInfallible<CharacterRangeVector>(*alloc);
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
                    builder->AddAtom(atom);
                    break;
                }
                widechar first_digit = Next();
                if (first_digit == '8' || first_digit == '9') {
                    // Treat as identity escape
                    builder->AddCharacter(first_digit);
                    Advance(2);
                    break;
                }
              }
                // FALLTHROUGH
              case '0': {
                Advance();
                size_t octal = ParseOctalLiteral();
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
                size_t value;
                if (ParseHexEscape(2, &value)) {
                    builder->AddCharacter(value);
                } else {
                    builder->AddCharacter('x');
                }
                break;
              }
              case 'u': {
                Advance(2);
                size_t value;
                if (ParseHexEscape(4, &value)) {
                    builder->AddCharacter(value);
                } else {
                    builder->AddCharacter('u');
                }
                break;
              }
              default:
                // Identity escape.
                builder->AddCharacter(Next());
                Advance(2);
                break;
            }
            break;
          case '{': {
            int dummy;
            if (ParseIntervalQuantifier(&dummy, &dummy))
                return ReportError(JSMSG_NOTHING_TO_REPEAT);
            // fallthrough
          }
          default:
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
ParsePattern(frontend::TokenStream& ts, LifoAlloc& alloc, const CharT* chars, size_t length,
             bool multiline, bool match_only, RegExpCompileData* data)
{
    if (match_only) {
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

    RegExpParser<CharT> parser(ts, &alloc, chars, chars + length, multiline);
    data->tree = parser.ParsePattern();
    if (!data->tree)
        return false;

    data->simple = parser.simple();
    data->contains_anchor = parser.contains_anchor();
    data->capture_count = parser.captures_started();
    return true;
}

bool
irregexp::ParsePattern(frontend::TokenStream& ts, LifoAlloc& alloc, JSAtom* str,
                       bool multiline, bool match_only,
                       RegExpCompileData* data)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::ParsePattern(ts, alloc, str->latin1Chars(nogc), str->length(),
                            multiline, match_only, data)
           : ::ParsePattern(ts, alloc, str->twoByteChars(nogc), str->length(),
                            multiline, match_only, data);
}

template <typename CharT>
static bool
ParsePatternSyntax(frontend::TokenStream& ts, LifoAlloc& alloc, const CharT* chars, size_t length)
{
    LifoAllocScope scope(&alloc);

    RegExpParser<CharT> parser(ts, &alloc, chars, chars + length, false);
    return parser.ParsePattern() != nullptr;
}

bool
irregexp::ParsePatternSyntax(frontend::TokenStream& ts, LifoAlloc& alloc, JSAtom* str)
{
    JS::AutoCheckCannotGC nogc;
    return str->hasLatin1Chars()
           ? ::ParsePatternSyntax(ts, alloc, str->latin1Chars(nogc), str->length())
           : ::ParsePatternSyntax(ts, alloc, str->twoByteChars(nogc), str->length());
}
