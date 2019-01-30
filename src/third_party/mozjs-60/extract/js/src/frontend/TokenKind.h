/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TokenKind_h
#define frontend_TokenKind_h

/*
 * List of token kinds and their ranges.
 *
 * The format for each line is:
 *
 *   macro(<TOKEN_KIND_NAME>, <DESCRIPTION>)
 *
 * or
 *
 *   range(<TOKEN_RANGE_NAME>, <TOKEN_KIND_NAME>)
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
 * Second argument of `range` is the actual value of the <TOKEN_RANGE_NAME>,
 * should be same as one of <TOKEN_KIND_NAME> in other `macro`s.
 *
 * To use this macro, define two macros for `macro` and `range`, and pass them
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
#define FOR_EACH_TOKEN_KIND_WITH_RANGE(macro, range) \
    macro(Eof,         "end of script") \
    \
    /* only returned by peekTokenSameLine() */ \
    macro(Eol,          "line terminator") \
    \
    macro(Semi,         "';'") \
    macro(Comma,        "','") \
    macro(Hook,         "'?'")    /* conditional */ \
    macro(Colon,        "':'")    /* conditional */ \
    macro(Inc,          "'++'")   /* increment */ \
    macro(Dec,          "'--'")   /* decrement */ \
    macro(Dot,          "'.'")    /* member operator */ \
    macro(TripleDot,    "'...'")  /* rest arguments and spread operator */ \
    macro(Lb,           "'['") \
    macro(Rb,           "']'") \
    macro(Lc,           "'{'") \
    macro(Rc,           "'}'") \
    macro(Lp,           "'('") \
    macro(Rp,           "')'") \
    macro(Name,         "identifier") \
    macro(Number,       "numeric literal") \
    macro(String,       "string literal") \
    \
    /* start of template literal with substitutions */ \
    macro(TemplateHead,    "'${'") \
    /* template literal without substitutions */ \
    macro(NoSubsTemplate, "template literal") \
    \
    macro(RegExp,       "regular expression literal") \
    macro(True,         "boolean literal 'true'") \
    range(ReservedWordLiteralFirst, True) \
    macro(False,        "boolean literal 'false'") \
    macro(Null,         "null literal") \
    range(ReservedWordLiteralLast, Null) \
    macro(This,         "keyword 'this'") \
    range(KeywordFirst, This) \
    macro(Function,     "keyword 'function'") \
    macro(If,           "keyword 'if'") \
    macro(Else,         "keyword 'else'") \
    macro(Switch,       "keyword 'switch'") \
    macro(Case,         "keyword 'case'") \
    macro(Default,      "keyword 'default'") \
    macro(While,        "keyword 'while'") \
    macro(Do,           "keyword 'do'") \
    macro(For,          "keyword 'for'") \
    macro(Break,        "keyword 'break'") \
    macro(Continue,     "keyword 'continue'") \
    macro(Var,          "keyword 'var'") \
    macro(Const,        "keyword 'const'") \
    macro(With,         "keyword 'with'") \
    macro(Return,       "keyword 'return'") \
    macro(New,          "keyword 'new'") \
    macro(Delete,       "keyword 'delete'") \
    macro(Try,          "keyword 'try'") \
    macro(Catch,        "keyword 'catch'") \
    macro(Finally,      "keyword 'finally'") \
    macro(Throw,        "keyword 'throw'") \
    macro(Debugger,     "keyword 'debugger'") \
    macro(Export,       "keyword 'export'") \
    macro(Import,       "keyword 'import'") \
    macro(Class,        "keyword 'class'") \
    macro(Extends,      "keyword 'extends'") \
    macro(Super,        "keyword 'super'") \
    range(KeywordLast, Super) \
    \
    /* contextual keywords */ \
    macro(As,           "'as'") \
    range(ContextualKeywordFirst, As) \
    macro(Async,        "'async'") \
    macro(Await,        "'await'") \
    macro(Each,         "'each'") \
    macro(From,         "'from'") \
    macro(Get,          "'get'") \
    macro(Let,          "'let'") \
    macro(Of,           "'of'") \
    macro(Set,          "'set'") \
    macro(Static,       "'static'") \
    macro(Target,       "'target'") \
    macro(Yield,        "'yield'") \
    range(ContextualKeywordLast, Yield) \
    \
    /* future reserved words */ \
    macro(Enum,         "reserved word 'enum'") \
    range(FutureReservedKeywordFirst, Enum) \
    range(FutureReservedKeywordLast, Enum) \
    \
    /* reserved words in strict mode */ \
    macro(Implements,   "reserved word 'implements'") \
    range(StrictReservedKeywordFirst, Implements) \
    macro(Interface,    "reserved word 'interface'") \
    macro(Package,      "reserved word 'package'") \
    macro(Private,      "reserved word 'private'") \
    macro(Protected,    "reserved word 'protected'") \
    macro(Public,       "reserved word 'public'") \
    range(StrictReservedKeywordLast, Public) \
    \
    /* \
     * The following token types occupy contiguous ranges to enable easy \
     * range-testing. \
     */ \
    /* \
     * Binary operators tokens, Or thru Pow. These must be in the same \
     * order as F(OR) and friends in FOR_EACH_PARSE_NODE_KIND in ParseNode.h. \
     */ \
    macro(Pipeline,     "'|>'") \
    range(BinOpFirst,   Pipeline) \
    macro(Or,           "'||'")   /* logical or */ \
    macro(And,          "'&&'")   /* logical and */ \
    macro(BitOr,        "'|'")    /* bitwise-or */ \
    macro(BitXor,       "'^'")    /* bitwise-xor */ \
    macro(BitAnd,       "'&'")    /* bitwise-and */ \
    \
    /* Equality operation tokens, per TokenKindIsEquality. */ \
    macro(StrictEq,     "'==='") \
    range(EqualityStart, StrictEq) \
    macro(Eq,           "'=='") \
    macro(StrictNe,     "'!=='") \
    macro(Ne,           "'!='") \
    range(EqualityLast, Ne) \
    \
    /* Relational ops, per TokenKindIsRelational. */ \
    macro(Lt,           "'<'") \
    range(RelOpStart,   Lt) \
    macro(Le,           "'<='") \
    macro(Gt,           "'>'") \
    macro(Ge,           "'>='") \
    range(RelOpLast,    Ge) \
    \
    macro(InstanceOf,   "keyword 'instanceof'") \
    range(KeywordBinOpFirst, InstanceOf) \
    macro(In,           "keyword 'in'") \
    range(KeywordBinOpLast, In) \
    \
    /* Shift ops, per TokenKindIsShift. */ \
    macro(Lsh,          "'<<'") \
    range(ShiftOpStart, Lsh) \
    macro(Rsh,          "'>>'") \
    macro(Ursh,         "'>>>'") \
    range(ShiftOpLast,  Ursh) \
    \
    macro(Add,          "'+'") \
    macro(Sub,          "'-'") \
    macro(Mul,          "'*'") \
    macro(Div,          "'/'") \
    macro(Mod,          "'%'") \
    macro(Pow,          "'**'") \
    range(BinOpLast,    Pow) \
    \
    /* Unary operation tokens. */ \
    macro(TypeOf,       "keyword 'typeof'") \
    range(KeywordUnOpFirst, TypeOf) \
    macro(Void,         "keyword 'void'") \
    range(KeywordUnOpLast, Void) \
    macro(Not,          "'!'") \
    macro(BitNot,       "'~'") \
    \
    macro(Arrow,        "'=>'")   /* function arrow */ \
    \
    /* Assignment ops, per TokenKindIsAssignment */ \
    macro(Assign,       "'='") \
    range(AssignmentStart, Assign) \
    macro(AddAssign,    "'+='") \
    macro(SubAssign,    "'-='") \
    macro(BitOrAssign,  "'|='") \
    macro(BitXorAssign, "'^='") \
    macro(BitAndAssign, "'&='") \
    macro(LshAssign,    "'<<='") \
    macro(RshAssign,    "'>>='") \
    macro(UrshAssign,   "'>>>='") \
    macro(MulAssign,    "'*='") \
    macro(DivAssign,    "'/='") \
    macro(ModAssign,    "'%='") \
    macro(PowAssign,    "'**='") \
    range(AssignmentLast, PowAssign)

#define TOKEN_KIND_RANGE_EMIT_NONE(name, value)
#define FOR_EACH_TOKEN_KIND(macro) \
    FOR_EACH_TOKEN_KIND_WITH_RANGE(macro, TOKEN_KIND_RANGE_EMIT_NONE)

namespace js {
namespace frontend {

// Values of this type are used to index into arrays such as isExprEnding[],
// so the first value must be zero.
enum class TokenKind {
#define EMIT_ENUM(name, desc) name,
#define EMIT_ENUM_RANGE(name, value) name = value,
    FOR_EACH_TOKEN_KIND_WITH_RANGE(EMIT_ENUM, EMIT_ENUM_RANGE)
#undef EMIT_ENUM
#undef EMIT_ENUM_RANGE
    Limit                      // domain size
};

inline bool
TokenKindIsBinaryOp(TokenKind tt)
{
    return TokenKind::BinOpFirst <= tt && tt <= TokenKind::BinOpLast;
}

inline bool
TokenKindIsEquality(TokenKind tt)
{
    return TokenKind::EqualityStart <= tt && tt <= TokenKind::EqualityLast;
}

inline bool
TokenKindIsRelational(TokenKind tt)
{
    return TokenKind::RelOpStart <= tt && tt <= TokenKind::RelOpLast;
}

inline bool
TokenKindIsShift(TokenKind tt)
{
    return TokenKind::ShiftOpStart <= tt && tt <= TokenKind::ShiftOpLast;
}

inline bool
TokenKindIsAssignment(TokenKind tt)
{
    return TokenKind::AssignmentStart <= tt && tt <= TokenKind::AssignmentLast;
}

inline MOZ_MUST_USE bool
TokenKindIsKeyword(TokenKind tt)
{
    return (TokenKind::KeywordFirst <= tt && tt <= TokenKind::KeywordLast) ||
           (TokenKind::KeywordBinOpFirst <= tt && tt <= TokenKind::KeywordBinOpLast) ||
           (TokenKind::KeywordUnOpFirst <= tt && tt <= TokenKind::KeywordUnOpLast);
}

inline MOZ_MUST_USE bool
TokenKindIsContextualKeyword(TokenKind tt)
{
    return TokenKind::ContextualKeywordFirst <= tt && tt <= TokenKind::ContextualKeywordLast;
}

inline MOZ_MUST_USE bool
TokenKindIsFutureReservedWord(TokenKind tt)
{
    return TokenKind::FutureReservedKeywordFirst <= tt && tt <= TokenKind::FutureReservedKeywordLast;
}

inline MOZ_MUST_USE bool
TokenKindIsStrictReservedWord(TokenKind tt)
{
    return TokenKind::StrictReservedKeywordFirst <= tt && tt <= TokenKind::StrictReservedKeywordLast;
}

inline MOZ_MUST_USE bool
TokenKindIsReservedWordLiteral(TokenKind tt)
{
    return TokenKind::ReservedWordLiteralFirst <= tt && tt <= TokenKind::ReservedWordLiteralLast;
}

inline MOZ_MUST_USE bool
TokenKindIsReservedWord(TokenKind tt)
{
    return TokenKindIsKeyword(tt) ||
           TokenKindIsFutureReservedWord(tt) ||
           TokenKindIsReservedWordLiteral(tt);
}

inline MOZ_MUST_USE bool
TokenKindIsPossibleIdentifier(TokenKind tt)
{
    return tt == TokenKind::Name ||
           TokenKindIsContextualKeyword(tt) ||
           TokenKindIsStrictReservedWord(tt);
}

inline MOZ_MUST_USE bool
TokenKindIsPossibleIdentifierName(TokenKind tt)
{
    return TokenKindIsPossibleIdentifier(tt) ||
           TokenKindIsReservedWord(tt);
}

} // namespace frontend
} // namespace js

#endif /* frontend_TokenKind_h */
