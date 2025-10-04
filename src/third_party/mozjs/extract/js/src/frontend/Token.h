/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Token-affiliated data structures except for TokenKind (defined in its own
 * header).
 */

#ifndef frontend_Token_h
#define frontend_Token_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint32_t

#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex, TrivialTaggedParserAtomIndex
#include "frontend/TokenKind.h"  // js::frontend::TokenKind
#include "js/RegExpFlags.h"      // JS::RegExpFlags

namespace js {

namespace frontend {

struct TokenPos {
  uint32_t begin = 0;  // Offset of the token's first code unit.
  uint32_t end = 0;    // Offset of 1 past the token's last code unit.

  TokenPos() = default;
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

  bool operator<(const TokenPos& bpos) const { return begin < bpos.begin; }

  bool operator<=(const TokenPos& bpos) const { return begin <= bpos.begin; }

  bool operator>(const TokenPos& bpos) const { return !(*this <= bpos); }

  bool operator>=(const TokenPos& bpos) const { return !(*this < bpos); }

  bool encloses(const TokenPos& pos) const {
    return begin <= pos.begin && pos.end <= end;
  }
};

enum DecimalPoint { NoDecimal = false, HasDecimal = true };

// The only escapes found in IdentifierName are of the Unicode flavor.
enum class IdentifierEscapes { None, SawUnicodeEscape };

enum class NameVisibility { Public, Private };

class TokenStreamShared;

struct Token {
 private:
  // The lexical grammar of JavaScript has a quirk around the '/' character.
  // As the spec puts it:
  //
  // > There are several situations where the identification of lexical input
  // > elements is sensitive to the syntactic grammar context that is consuming
  // > the input elements. This requires multiple goal symbols for the lexical
  // > grammar. [...] The InputElementRegExp goal symbol is used in all
  // > syntactic grammar contexts where a RegularExpressionLiteral is permitted
  // > [...]  In all other contexts, InputElementDiv is used as the lexical
  // > goal symbol.
  //
  // https://tc39.github.io/ecma262/#sec-lexical-and-regexp-grammars
  //
  // What "sensitive to the syntactic grammar context" means is, the parser has
  // to tell the TokenStream whether to interpret '/' as division or
  // RegExp. Because only one or the other (or neither) will be legal at that
  // point in the program, and only the parser knows which one.
  //
  // But there's a problem: the parser often gets a token, puts it back, then
  // consumes it later; or (equivalently) peeks at a token, leaves it, peeks
  // again later, then finally consumes it. Of course we don't actually re-scan
  // the token every time; we cache it in the TokenStream. This leads to the
  // following rule:
  //
  // The parser must not pass SlashIsRegExp when getting/peeking at a token
  // previously scanned with SlashIsDiv; or vice versa.
  //
  // That way, code that asks for a SlashIsRegExp mode will never get a cached
  // Div token. But this rule is easy to screw up, because tokens are so often
  // peeked at on Parser.cpp line A and consumed on line B, where |A-B| is
  // thousands of lines. We therefore enforce it with the frontend's most
  // annoying assertion (in verifyConsistentModifier), and provide
  // Modifier::SlashIsInvalid to help avoid tripping it.
  //
  // This enum belongs in TokenStream, but C++, so we define it here and
  // typedef it there.
  enum Modifier {
    // Parse `/` and `/=` as the division operators. (That is, use
    // InputElementDiv as the goal symbol.)
    SlashIsDiv,

    // Parse `/` as the beginning of a RegExp literal. (That is, use
    // InputElementRegExp.)
    SlashIsRegExp,

    // Neither a Div token nor a RegExp token is syntactically valid here. When
    // the parser calls `getToken(SlashIsInvalid)`, it must be prepared to see
    // either one (and throw a SyntaxError either way).
    //
    // It's OK to use SlashIsInvalid to get a token that was originally scanned
    // with SlashIsDiv or SlashIsRegExp. The reverse--peeking with
    // SlashIsInvalid, then getting with another mode--is not OK. If either Div
    // or RegExp is syntactically valid here, use the appropriate modifier.
    SlashIsInvalid,
  };
  friend class TokenStreamShared;

 public:
  /** The type of this token. */
  TokenKind type;

  /** The token's position in the overall script. */
  TokenPos pos;

  union {
   private:
    friend struct Token;

    TrivialTaggedParserAtomIndex atom;

    struct {
      /** Numeric literal's value. */
      double value;

      /** Does the numeric literal contain a '.'? */
      DecimalPoint decimalPoint;
    } number;

    /** Regular expression flags; use charBuffer to access source chars. */
    JS::RegExpFlags reflags;
  } u;

#ifdef DEBUG
  /** The modifier used to get this token. */
  Modifier modifier;
#endif

  // Mutators

  void setName(TaggedParserAtomIndex name) {
    MOZ_ASSERT(type == TokenKind::Name || type == TokenKind::PrivateName);
    u.atom = TrivialTaggedParserAtomIndex::from(name);
  }

  void setAtom(TaggedParserAtomIndex atom) {
    MOZ_ASSERT(type == TokenKind::String || type == TokenKind::TemplateHead ||
               type == TokenKind::NoSubsTemplate);
    u.atom = TrivialTaggedParserAtomIndex::from(atom);
  }

  void setRegExpFlags(JS::RegExpFlags flags) {
    MOZ_ASSERT(type == TokenKind::RegExp);
    u.reflags = flags;
  }

  void setNumber(double n, DecimalPoint decimalPoint) {
    MOZ_ASSERT(type == TokenKind::Number);
    u.number.value = n;
    u.number.decimalPoint = decimalPoint;
  }

  // Type-safe accessors

  TaggedParserAtomIndex name() const {
    MOZ_ASSERT(type == TokenKind::Name || type == TokenKind::PrivateName);
    return u.atom;
  }

  TaggedParserAtomIndex atom() const {
    MOZ_ASSERT(type == TokenKind::String || type == TokenKind::TemplateHead ||
               type == TokenKind::NoSubsTemplate);
    return u.atom;
  }

  JS::RegExpFlags regExpFlags() const {
    MOZ_ASSERT(type == TokenKind::RegExp);
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

}  // namespace frontend

}  // namespace js

#endif  // frontend_Token_h
