/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TokenKind_h
#define frontend_TokenKind_h

#include <stdint.h>

/*
 * List of token kinds and their ranges.
 *
 * The format for each line is:
 *
 *   MACRO(<TOKEN_KIND_NAME>, <DESCRIPTION>)
 *
 * or
 *
 *   RANGE(<TOKEN_RANGE_NAME>, <TOKEN_KIND_NAME>)
 *
 * where ;
 * <TOKEN_KIND_NAME> is a legal C identifier of the token, that will be used in
 * the JS engine source.
 *
 * <DESCRIPTION> is a string that describe about the token, and will be used in
 * error message.
 *
 * <TOKEN_RANGE_NAME> is a legal C identifier of the range that will be used to
 * JS engine source. It should end with `First` or `Last`. This is used to check
 * TokenKind by range-testing:
 *   BinOpFirst <= tt && tt <= BinOpLast
 *
 * Second argument of `RANGE` is the actual value of the <TOKEN_RANGE_NAME>,
 * should be same as one of <TOKEN_KIND_NAME> in other `MACRO`s.
 *
 * To use this macro, define two macros for `MACRO` and `RANGE`, and pass them
 * as arguments.
 *
 *   #define EMIT_TOKEN(name, desc) ...
 *   #define EMIT_RANGE(name, value) ...
 *   FOR_EACH_TOKEN_KIND_WITH_RANGE(EMIT_TOKEN, EMIT_RANGE)
 *   #undef EMIT_TOKEN
 *   #undef EMIT_RANGE
 *
 * If you don't need range data, use FOR_EACH_TOKEN_KIND instead.
 *
 *   #define EMIT_TOKEN(name, desc) ...
 *   FOR_EACH_TOKEN_KIND(EMIT_TOKEN)
 *   #undef EMIT_TOKEN
 *
 * Note that this list does not contain ERROR and LIMIT.
 */
#define FOR_EACH_TOKEN_KIND_WITH_RANGE(MACRO, RANGE)                   \
  MACRO(Eof, "end of script")                                          \
                                                                       \
  /* only returned by peekTokenSameLine() */                           \
  MACRO(Eol, "line terminator")                                        \
                                                                       \
  MACRO(Semi, "';'")                                                   \
  MACRO(Comma, "','")                                                  \
  MACRO(Hook, "'?'")        /* conditional */                          \
  MACRO(Colon, "':'")       /* conditional */                          \
  MACRO(Inc, "'++'")        /* increment */                            \
  MACRO(Dec, "'--'")        /* decrement */                            \
  MACRO(Dot, "'.'")         /* member operator */                      \
  MACRO(TripleDot, "'...'") /* rest arguments and spread operator */   \
  MACRO(OptionalChain, "'?.'")                                         \
  MACRO(LeftBracket, "'['")                                            \
  MACRO(RightBracket, "']'")                                           \
  MACRO(LeftCurly, "'{'")                                              \
  MACRO(RightCurly, "'}'")                                             \
  MACRO(LeftParen, "'('")                                              \
  MACRO(RightParen, "')'")                                             \
  MACRO(Name, "identifier")                                            \
  MACRO(PrivateName, "private identifier")                             \
  MACRO(Number, "numeric literal")                                     \
  MACRO(String, "string literal")                                      \
  MACRO(BigInt, "bigint literal")                                      \
                                                                       \
  /* start of template literal with substitutions */                   \
  MACRO(TemplateHead, "'${'")                                          \
  /* template literal without substitutions */                         \
  MACRO(NoSubsTemplate, "template literal")                            \
                                                                       \
  MACRO(RegExp, "regular expression literal")                          \
  MACRO(True, "boolean literal 'true'")                                \
  RANGE(ReservedWordLiteralFirst, True)                                \
  MACRO(False, "boolean literal 'false'")                              \
  MACRO(Null, "null literal")                                          \
  RANGE(ReservedWordLiteralLast, Null)                                 \
  MACRO(This, "keyword 'this'")                                        \
  RANGE(KeywordFirst, This)                                            \
  MACRO(Function, "keyword 'function'")                                \
  MACRO(If, "keyword 'if'")                                            \
  MACRO(Else, "keyword 'else'")                                        \
  MACRO(Switch, "keyword 'switch'")                                    \
  MACRO(Case, "keyword 'case'")                                        \
  MACRO(Default, "keyword 'default'")                                  \
  MACRO(While, "keyword 'while'")                                      \
  MACRO(Do, "keyword 'do'")                                            \
  MACRO(For, "keyword 'for'")                                          \
  MACRO(Break, "keyword 'break'")                                      \
  MACRO(Continue, "keyword 'continue'")                                \
  MACRO(Var, "keyword 'var'")                                          \
  MACRO(Const, "keyword 'const'")                                      \
  MACRO(With, "keyword 'with'")                                        \
  MACRO(Return, "keyword 'return'")                                    \
  MACRO(New, "keyword 'new'")                                          \
  MACRO(Delete, "keyword 'delete'")                                    \
  MACRO(Try, "keyword 'try'")                                          \
  MACRO(Catch, "keyword 'catch'")                                      \
  MACRO(Finally, "keyword 'finally'")                                  \
  MACRO(Throw, "keyword 'throw'")                                      \
  MACRO(Debugger, "keyword 'debugger'")                                \
  MACRO(Export, "keyword 'export'")                                    \
  MACRO(Import, "keyword 'import'")                                    \
  MACRO(Class, "keyword 'class'")                                      \
  MACRO(Extends, "keyword 'extends'")                                  \
  MACRO(Super, "keyword 'super'")                                      \
  RANGE(KeywordLast, Super)                                            \
                                                                       \
  /* contextual keywords */                                            \
  MACRO(As, "'as'")                                                    \
  RANGE(ContextualKeywordFirst, As)                                    \
  MACRO(Async, "'async'")                                              \
  MACRO(Await, "'await'")                                              \
  MACRO(Each, "'each'")                                                \
  MACRO(From, "'from'")                                                \
  MACRO(Get, "'get'")                                                  \
  MACRO(Let, "'let'")                                                  \
  MACRO(Meta, "'meta'")                                                \
  MACRO(Of, "'of'")                                                    \
  MACRO(Set, "'set'")                                                  \
  MACRO(Static, "'static'")                                            \
  MACRO(Target, "'target'")                                            \
  MACRO(Yield, "'yield'")                                              \
  RANGE(ContextualKeywordLast, Yield)                                  \
                                                                       \
  /* future reserved words */                                          \
  MACRO(Enum, "reserved word 'enum'")                                  \
  RANGE(FutureReservedKeywordFirst, Enum)                              \
  RANGE(FutureReservedKeywordLast, Enum)                               \
                                                                       \
  /* reserved words in strict mode */                                  \
  MACRO(Implements, "reserved word 'implements'")                      \
  RANGE(StrictReservedKeywordFirst, Implements)                        \
  MACRO(Interface, "reserved word 'interface'")                        \
  MACRO(Package, "reserved word 'package'")                            \
  MACRO(Private, "reserved word 'private'")                            \
  MACRO(Protected, "reserved word 'protected'")                        \
  MACRO(Public, "reserved word 'public'")                              \
  RANGE(StrictReservedKeywordLast, Public)                             \
                                                                       \
  /*                                                                   \
   * The following token types occupy contiguous ranges to enable easy \
   * range-testing.                                                    \
   */                                                                  \
  /*                                                                   \
   * Binary operators.                                                 \
   * This list must be kept in the same order in several places:       \
   *   - the binary operators in ParseNode.h                           \
   *   - the precedence list in Parser.cpp                             \
   *   - the JSOp code list in BytecodeEmitter.cpp                     \
   */                                                                  \
  MACRO(Coalesce, "'\?\?'") /* escapes to avoid trigraphs warning */   \
  RANGE(BinOpFirst, Coalesce)                                          \
  MACRO(Or, "'||'")    /* logical or */                                \
  MACRO(And, "'&&'")   /* logical and */                               \
  MACRO(BitOr, "'|'")  /* bitwise-or */                                \
  MACRO(BitXor, "'^'") /* bitwise-xor */                               \
  MACRO(BitAnd, "'&'") /* bitwise-and */                               \
                                                                       \
  /* Equality operation tokens, per TokenKindIsEquality. */            \
  MACRO(StrictEq, "'==='")                                             \
  RANGE(EqualityStart, StrictEq)                                       \
  MACRO(Eq, "'=='")                                                    \
  MACRO(StrictNe, "'!=='")                                             \
  MACRO(Ne, "'!='")                                                    \
  RANGE(EqualityLast, Ne)                                              \
                                                                       \
  /* Relational ops, per TokenKindIsRelational. */                     \
  MACRO(Lt, "'<'")                                                     \
  RANGE(RelOpStart, Lt)                                                \
  MACRO(Le, "'<='")                                                    \
  MACRO(Gt, "'>'")                                                     \
  MACRO(Ge, "'>='")                                                    \
  RANGE(RelOpLast, Ge)                                                 \
                                                                       \
  MACRO(InstanceOf, "keyword 'instanceof'")                            \
  RANGE(KeywordBinOpFirst, InstanceOf)                                 \
  MACRO(In, "keyword 'in'")                                            \
  MACRO(PrivateIn, "keyword 'in' (private)")                           \
  RANGE(KeywordBinOpLast, PrivateIn)                                   \
                                                                       \
  /* Shift ops, per TokenKindIsShift. */                               \
  MACRO(Lsh, "'<<'")                                                   \
  RANGE(ShiftOpStart, Lsh)                                             \
  MACRO(Rsh, "'>>'")                                                   \
  MACRO(Ursh, "'>>>'")                                                 \
  RANGE(ShiftOpLast, Ursh)                                             \
                                                                       \
  MACRO(Add, "'+'")                                                    \
  MACRO(Sub, "'-'")                                                    \
  MACRO(Mul, "'*'")                                                    \
  MACRO(Div, "'/'")                                                    \
  MACRO(Mod, "'%'")                                                    \
  MACRO(Pow, "'**'")                                                   \
  RANGE(BinOpLast, Pow)                                                \
                                                                       \
  /* Unary operation tokens. */                                        \
  MACRO(TypeOf, "keyword 'typeof'")                                    \
  RANGE(KeywordUnOpFirst, TypeOf)                                      \
  MACRO(Void, "keyword 'void'")                                        \
  RANGE(KeywordUnOpLast, Void)                                         \
  MACRO(Not, "'!'")                                                    \
  MACRO(BitNot, "'~'")                                                 \
                                                                       \
  MACRO(Arrow, "'=>'") /* function arrow */                            \
                                                                       \
  /* Assignment ops, per TokenKindIsAssignment */                      \
  MACRO(Assign, "'='")                                                 \
  RANGE(AssignmentStart, Assign)                                       \
  MACRO(AddAssign, "'+='")                                             \
  MACRO(SubAssign, "'-='")                                             \
  MACRO(CoalesceAssign, "'\?\?='") /* avoid trigraphs warning */       \
  MACRO(OrAssign, "'||='")                                             \
  MACRO(AndAssign, "'&&='")                                            \
  MACRO(BitOrAssign, "'|='")                                           \
  MACRO(BitXorAssign, "'^='")                                          \
  MACRO(BitAndAssign, "'&='")                                          \
  MACRO(LshAssign, "'<<='")                                            \
  MACRO(RshAssign, "'>>='")                                            \
  MACRO(UrshAssign, "'>>>='")                                          \
  MACRO(MulAssign, "'*='")                                             \
  MACRO(DivAssign, "'/='")                                             \
  MACRO(ModAssign, "'%='")                                             \
  MACRO(PowAssign, "'**='")                                            \
  RANGE(AssignmentLast, PowAssign)

#define TOKEN_KIND_RANGE_EMIT_NONE(name, value)
#define FOR_EACH_TOKEN_KIND(MACRO) \
  FOR_EACH_TOKEN_KIND_WITH_RANGE(MACRO, TOKEN_KIND_RANGE_EMIT_NONE)

namespace js {
namespace frontend {

// Values of this type are used to index into arrays such as isExprEnding[],
// so the first value must be zero.
enum class TokenKind : uint8_t {
#define EMIT_ENUM(name, desc) name,
#define EMIT_ENUM_RANGE(name, value) name = value,
  FOR_EACH_TOKEN_KIND_WITH_RANGE(EMIT_ENUM, EMIT_ENUM_RANGE)
#undef EMIT_ENUM
#undef EMIT_ENUM_RANGE
      Limit  // domain size
};

inline bool TokenKindIsBinaryOp(TokenKind tt) {
  return TokenKind::BinOpFirst <= tt && tt <= TokenKind::BinOpLast;
}

inline bool TokenKindIsEquality(TokenKind tt) {
  return TokenKind::EqualityStart <= tt && tt <= TokenKind::EqualityLast;
}

inline bool TokenKindIsRelational(TokenKind tt) {
  return TokenKind::RelOpStart <= tt && tt <= TokenKind::RelOpLast;
}

inline bool TokenKindIsShift(TokenKind tt) {
  return TokenKind::ShiftOpStart <= tt && tt <= TokenKind::ShiftOpLast;
}

inline bool TokenKindIsAssignment(TokenKind tt) {
  return TokenKind::AssignmentStart <= tt && tt <= TokenKind::AssignmentLast;
}

[[nodiscard]] inline bool TokenKindIsKeyword(TokenKind tt) {
  return (TokenKind::KeywordFirst <= tt && tt <= TokenKind::KeywordLast) ||
         (TokenKind::KeywordBinOpFirst <= tt &&
          tt <= TokenKind::KeywordBinOpLast) ||
         (TokenKind::KeywordUnOpFirst <= tt &&
          tt <= TokenKind::KeywordUnOpLast);
}

[[nodiscard]] inline bool TokenKindIsContextualKeyword(TokenKind tt) {
  return TokenKind::ContextualKeywordFirst <= tt &&
         tt <= TokenKind::ContextualKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsFutureReservedWord(TokenKind tt) {
  return TokenKind::FutureReservedKeywordFirst <= tt &&
         tt <= TokenKind::FutureReservedKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsStrictReservedWord(TokenKind tt) {
  return TokenKind::StrictReservedKeywordFirst <= tt &&
         tt <= TokenKind::StrictReservedKeywordLast;
}

[[nodiscard]] inline bool TokenKindIsReservedWordLiteral(TokenKind tt) {
  return TokenKind::ReservedWordLiteralFirst <= tt &&
         tt <= TokenKind::ReservedWordLiteralLast;
}

[[nodiscard]] inline bool TokenKindIsReservedWord(TokenKind tt) {
  return TokenKindIsKeyword(tt) || TokenKindIsFutureReservedWord(tt) ||
         TokenKindIsReservedWordLiteral(tt);
}

[[nodiscard]] inline bool TokenKindIsPossibleIdentifier(TokenKind tt) {
  return tt == TokenKind::Name || TokenKindIsContextualKeyword(tt) ||
         TokenKindIsStrictReservedWord(tt);
}

[[nodiscard]] inline bool TokenKindIsPossibleIdentifierName(TokenKind tt) {
  return TokenKindIsPossibleIdentifier(tt) || TokenKindIsReservedWord(tt);
}

}  // namespace frontend
}  // namespace js

#endif /* frontend_TokenKind_h */
