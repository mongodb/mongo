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

#ifndef V8_PARSER_H_
#define V8_PARSER_H_

#include "mozilla/Range.h"

#include <stdarg.h>

#include "irregexp/RegExpAST.h"

namespace js {

namespace frontend {
    class TokenStreamAnyChars;
}

namespace irregexp {

extern bool
ParsePattern(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc, JSAtom* str,
             bool multiline, bool match_only, bool unicode, bool ignore_case,
             bool global, bool sticky, RegExpCompileData* data);

extern bool
ParsePatternSyntax(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc, JSAtom* str,
                   bool unicode);

extern bool
ParsePatternSyntax(frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc,
                   const mozilla::Range<const char16_t> chars, bool unicode);

// A BufferedVector is an automatically growing list, just like (and backed
// by) a Vector, that is optimized for the case of adding and removing
// a single element. The last element added is stored outside the backing list,
// and if no more than one element is ever added, the ZoneList isn't even
// allocated.
// Elements must not be nullptr pointers.
template <typename T, int initial_size>
class BufferedVector
{
  public:
    typedef InfallibleVector<T*, 1> VectorType;

    BufferedVector() : list_(nullptr), last_(nullptr) {}

    // Adds element at end of list. This element is buffered and can
    // be read using last() or removed using RemoveLast until a new Add or until
    // RemoveLast or GetList has been called.
    void Add(LifoAlloc* alloc, T* value) {
        if (last_ != nullptr) {
            if (list_ == nullptr) {
                list_ = alloc->newInfallible<VectorType>(*alloc);
                list_->reserve(initial_size);
            }
            list_->append(last_);
        }
        last_ = value;
    }

    T* last() {
        MOZ_ASSERT(last_ != nullptr);
        return last_;
    }

    T* RemoveLast() {
        MOZ_ASSERT(last_ != nullptr);
        T* result = last_;
        if ((list_ != nullptr) && (list_->length() > 0))
            last_ = list_->popCopy();
        else
            last_ = nullptr;
        return result;
    }

    T* Get(int i) {
        MOZ_ASSERT((0 <= i) && (i < length()));
        if (list_ == nullptr) {
            MOZ_ASSERT(0 == i);
            return last_;
        } else {
            if (size_t(i) == list_->length()) {
                MOZ_ASSERT(last_ != nullptr);
                return last_;
            } else {
                return (*list_)[i];
            }
        }
    }

    void Clear() {
        list_ = nullptr;
        last_ = nullptr;
    }

    int length() {
        int length = (list_ == nullptr) ? 0 : list_->length();
        return length + ((last_ == nullptr) ? 0 : 1);
    }

    VectorType* GetList(LifoAlloc* alloc) {
        if (list_ == nullptr)
            list_ = alloc->newInfallible<VectorType>(*alloc);
        if (last_ != nullptr) {
            list_->append(last_);
            last_ = nullptr;
        }
        return list_;
    }

  private:
    VectorType* list_;
    T* last_;
};


// Accumulates RegExp atoms and assertions into lists of terms and alternatives.
class RegExpBuilder
{
  public:
    explicit RegExpBuilder(LifoAlloc* alloc);
    void AddCharacter(char16_t character);
    // "Adds" an empty expression. Does nothing except consume a
    // following quantifier
    void AddEmpty();
    void AddAtom(RegExpTree* tree);
    void AddAssertion(RegExpTree* tree);
    void NewAlternative();  // '|'
    void AddQuantifierToAtom(int min, int max, RegExpQuantifier::QuantifierType type);
    RegExpTree* ToRegExp();

  private:
    void FlushCharacters();
    void FlushText();
    void FlushTerms();

    LifoAlloc* alloc;
    bool pending_empty_;
    CharacterVector* characters_;
    BufferedVector<RegExpTree, 2> terms_;
    BufferedVector<RegExpTree, 2> text_;
    BufferedVector<RegExpTree, 2> alternatives_;

    enum LastAdded {
        ADD_NONE, ADD_CHAR, ADD_TERM, ADD_ASSERT, ADD_ATOM
    };
#ifdef DEBUG
    LastAdded last_added_;
#endif
};

// Characters parsed by RegExpParser can be either char16_t or kEndMarker.
typedef uint32_t widechar;

template <typename CharT>
class RegExpParser
{
  public:
    RegExpParser(frontend::TokenStreamAnyChars& ts, LifoAlloc* alloc,
                 const CharT* chars, const CharT* end, bool multiline_mode, bool unicode,
                 bool ignore_case);

    RegExpTree* ParsePattern();
    RegExpTree* ParseDisjunction();
    RegExpTree* ParseCharacterClass();

    // Parses a {...,...} quantifier and stores the range in the given
    // out parameters.
    bool ParseIntervalQuantifier(int* min_out, int* max_out);

    // Tries to parse the input as a single escaped character.  If successful
    // it stores the result in the output parameter and returns true.
    // Otherwise it throws an error and returns false.  The character must not
    // be 'b' or 'B' since they are usually handled specially.
    bool ParseClassCharacterEscape(widechar* code);

    // Checks whether the following is a length-digit hexadecimal number,
    // and sets the value if it is.
    bool ParseHexEscape(int length, widechar* value);

    bool ParseBracedHexEscape(widechar* value);
    bool ParseTrailSurrogate(widechar* value);
    bool ParseRawSurrogatePair(char16_t* lead, char16_t* trail);

    widechar ParseOctalLiteral();

    // Tries to parse the input as a back reference.  If successful it
    // stores the result in the output parameter and returns true.  If
    // it fails it will push back the characters read so the same characters
    // can be reparsed.
    bool ParseBackReferenceIndex(int* index_out);

    bool ParseClassAtom(char16_t* char_class, widechar *value);

  private:
    void SyntaxError(unsigned errorNumber, ...);

  public:
    RegExpTree* ReportError(unsigned errorNumber, const char* param = nullptr);

    void Advance();
    void Advance(int dist) {
        next_pos_ += dist - 1;
        Advance();
    }

    void Reset(const CharT* pos) {
        next_pos_ = pos;
        has_more_ = (pos < end_);
        Advance();
    }

    // Reports whether the pattern might be used as a literal search string.
    // Only use if the result of the parse is a single atom node.
    bool simple() { return simple_; }
    bool contains_anchor() { return contains_anchor_; }
    void set_contains_anchor() { contains_anchor_ = true; }
    int captures_started() { return captures_ == nullptr ? 0 : captures_->length(); }
    const CharT* position() { return next_pos_ - 1; }

    static const int kMaxCaptures = 1 << 16;
    static const widechar kEndMarker = (1 << 21);

  private:
    enum SubexpressionType {
        INITIAL,
        CAPTURE,  // All positive values represent captures.
        POSITIVE_LOOKAHEAD,
        NEGATIVE_LOOKAHEAD,
        GROUPING
    };

    class RegExpParserState {
      public:
        RegExpParserState(LifoAlloc* alloc,
                          RegExpParserState* previous_state,
                          SubexpressionType group_type,
                          int disjunction_capture_index)
            : previous_state_(previous_state),
              builder_(alloc->newInfallible<RegExpBuilder>(alloc)),
              group_type_(group_type),
              disjunction_capture_index_(disjunction_capture_index)
        {}
        // Parser state of containing expression, if any.
        RegExpParserState* previous_state() { return previous_state_; }
        bool IsSubexpression() { return previous_state_ != nullptr; }
        // RegExpBuilder building this regexp's AST.
        RegExpBuilder* builder() { return builder_; }
        // Type of regexp being parsed (parenthesized group or entire regexp).
        SubexpressionType group_type() { return group_type_; }
        // Index in captures array of first capture in this sub-expression, if any.
        // Also the capture index of this sub-expression itself, if group_type
        // is CAPTURE.
        int capture_index() { return disjunction_capture_index_; }

      private:
        // Linked list implementation of stack of states.
        RegExpParserState* previous_state_;
        // Builder for the stored disjunction.
        RegExpBuilder* builder_;
        // Stored disjunction type (capture, look-ahead or grouping), if any.
        SubexpressionType group_type_;
        // Stored disjunction's capture index (if any).
        int disjunction_capture_index_;
    };

    widechar current() { return current_; }
    bool has_more() { return has_more_; }
    bool has_next() { return next_pos_ < end_; }
    widechar Next() {
        if (has_next())
            return *next_pos_;
        return kEndMarker;
    }
    void ScanForCaptures();

    frontend::TokenStreamAnyChars& ts;
    LifoAlloc* alloc;
    RegExpCaptureVector* captures_;
    const CharT* const start_;
    const CharT* next_pos_;
    const CharT* end_;
    widechar current_;
    // The capture count is only valid after we have scanned for captures.
    int capture_count_;
    bool has_more_;
    bool multiline_;
    bool unicode_;
    bool ignore_case_;
    bool simple_;
    bool contains_anchor_;
    bool is_scanned_for_captures_;
};

} } // namespace js::irregexp

#endif // V8_PARSER_H_
