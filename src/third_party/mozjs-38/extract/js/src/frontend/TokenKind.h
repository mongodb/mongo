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
 * the JS engine source, with `TOK_` prefix.
 *
 * <DESCRIPTION> is a string that describe about the token, and will be used in
 * error message.
 *
 * <TOKEN_RANGE_NAME> is a legal C identifier of the range that will be used to
 * JS engine source, with `TOK_` prefix. It should end with `_FIRST` or `_LAST`.
 * This is used to check TokenKind by range-testing:
 *   TOK_BINOP_FIRST <= tt && tt <= TOK_BINOP_LAST
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
    macro(EOF,         "end of script") \
    \
    /* only returned by peekTokenSameLine() */ \
    macro(EOL,          "line terminator") \
    \
    macro(SEMI,         "';'") \
    macro(COMMA,        "','") \
    macro(HOOK,         "'?'")    /* conditional */ \
    macro(COLON,        "':'")    /* conditional */ \
    macro(INC,          "'++'")   /* increment */ \
    macro(DEC,          "'--'")   /* decrement */ \
    macro(DOT,          "'.'")    /* member operator */ \
    macro(TRIPLEDOT,    "'...'")  /* rest arguments and spread operator */ \
    macro(LB,           "'['") \
    macro(RB,           "']'") \
    macro(LC,           "'{'") \
    macro(RC,           "'}'") \
    macro(LP,           "'('") \
    macro(RP,           "')'") \
    macro(NAME,         "identifier") \
    macro(NUMBER,       "numeric literal") \
    macro(STRING,       "string literal") \
    \
    /* start of template literal with substitutions */ \
    macro(TEMPLATE_HEAD,    "'${'") \
    /* template literal without substitutions */ \
    macro(NO_SUBS_TEMPLATE, "template literal") \
    \
    macro(REGEXP,       "regular expression literal") \
    macro(TRUE,         "boolean literal 'true'") \
    macro(FALSE,        "boolean literal 'false'") \
    macro(NULL,         "null literal") \
    macro(THIS,         "keyword 'this'") \
    macro(FUNCTION,     "keyword 'function'") \
    macro(IF,           "keyword 'if'") \
    macro(ELSE,         "keyword 'else'") \
    macro(SWITCH,       "keyword 'switch'") \
    macro(CASE,         "keyword 'case'") \
    macro(DEFAULT,      "keyword 'default'") \
    macro(WHILE,        "keyword 'while'") \
    macro(DO,           "keyword 'do'") \
    macro(FOR,          "keyword 'for'") \
    macro(BREAK,        "keyword 'break'") \
    macro(CONTINUE,     "keyword 'continue'") \
    macro(VAR,          "keyword 'var'") \
    macro(CONST,        "keyword 'const'") \
    macro(WITH,         "keyword 'with'") \
    macro(RETURN,       "keyword 'return'") \
    macro(NEW,          "keyword 'new'") \
    macro(DELETE,       "keyword 'delete'") \
    macro(TRY,          "keyword 'try'") \
    macro(CATCH,        "keyword 'catch'") \
    macro(FINALLY,      "keyword 'finally'") \
    macro(THROW,        "keyword 'throw'") \
    macro(DEBUGGER,     "keyword 'debugger'") \
    macro(YIELD,        "keyword 'yield'") \
    macro(LET,          "keyword 'let'") \
    macro(EXPORT,       "keyword 'export'") \
    macro(IMPORT,       "keyword 'import'") \
    macro(RESERVED,     "reserved keyword") \
    /* reserved keywords in strict mode */ \
    macro(STRICT_RESERVED, "reserved keyword") \
    \
    /* \
     * The following token types occupy contiguous ranges to enable easy \
     * range-testing. \
     */ \
    /* \
     * Binary operators tokens, TOK_OR thru TOK_MOD. These must be in the same \
     * order as F(OR) and friends in FOR_EACH_PARSE_NODE_KIND in ParseNode.h. \
     */ \
    macro(OR,           "'||'")   /* logical or */ \
    range(BINOP_FIRST, OR) \
    macro(AND,          "'&&'")   /* logical and */ \
    macro(BITOR,        "'|'")    /* bitwise-or */ \
    macro(BITXOR,       "'^'")    /* bitwise-xor */ \
    macro(BITAND,       "'&'")    /* bitwise-and */ \
    \
    /* Equality operation tokens, per TokenKindIsEquality. */ \
    macro(STRICTEQ,     "'==='") \
    range(EQUALITY_START, STRICTEQ) \
    macro(EQ,           "'=='") \
    macro(STRICTNE,     "'!=='") \
    macro(NE,           "'!='") \
    range(EQUALITY_LAST, NE) \
    \
    /* Relational ops, per TokenKindIsRelational. */ \
    macro(LT,           "'<'") \
    range(RELOP_START, LT) \
    macro(LE,           "'<='") \
    macro(GT,           "'>'") \
    macro(GE,           "'>='") \
    range(RELOP_LAST, GE) \
    \
    macro(INSTANCEOF,   "keyword 'instanceof'") \
    macro(IN,           "keyword 'in'") \
    \
    /* Shift ops, per TokenKindIsShift. */ \
    macro(LSH,          "'<<'") \
    range(SHIFTOP_START, LSH) \
    macro(RSH,          "'>>'") \
    macro(URSH,         "'>>>'") \
    range(SHIFTOP_LAST, URSH) \
    \
    macro(ADD,          "'+'") \
    macro(SUB,          "'-'") \
    macro(MUL,          "'*'") \
    macro(DIV,          "'/'") \
    macro(MOD,          "'%'") \
    range(BINOP_LAST, MOD) \
    \
    /* Unary operation tokens. */ \
    macro(TYPEOF,       "keyword 'typeof'") \
    macro(VOID,         "keyword 'void'") \
    macro(NOT,          "'!'") \
    macro(BITNOT,       "'~'") \
    \
    macro(ARROW,        "'=>'")   /* function arrow */ \
    \
    /* Assignment ops, per TokenKindIsAssignment */ \
    macro(ASSIGN,       "'='") \
    range(ASSIGNMENT_START, ASSIGN) \
    macro(ADDASSIGN,    "'+='") \
    macro(SUBASSIGN,    "'-='") \
    macro(BITORASSIGN,  "'|='") \
    macro(BITXORASSIGN, "'^='") \
    macro(BITANDASSIGN, "'&='") \
    macro(LSHASSIGN,    "'<<='") \
    macro(RSHASSIGN,    "'>>='") \
    macro(URSHASSIGN,   "'>>>='") \
    macro(MULASSIGN,    "'*='") \
    macro(DIVASSIGN,    "'/='") \
    macro(MODASSIGN,    "'%='") \
    range(ASSIGNMENT_LAST, MODASSIGN)

#define TOKEN_KIND_RANGE_EMIT_NONE(name, value)
#define FOR_EACH_TOKEN_KIND(macro) \
    FOR_EACH_TOKEN_KIND_WITH_RANGE(macro, TOKEN_KIND_RANGE_EMIT_NONE)

namespace js {
namespace frontend {

// Values of this type are used to index into arrays such as isExprEnding[],
// so the first value must be zero.
enum TokenKind {
#define EMIT_ENUM(name, desc) TOK_##name,
#define EMIT_ENUM_RANGE(name, value) TOK_##name = TOK_##value,
    FOR_EACH_TOKEN_KIND_WITH_RANGE(EMIT_ENUM, EMIT_ENUM_RANGE)
#undef EMIT_ENUM
#undef EMIT_ENUM_RANGE
    TOK_LIMIT                      // domain size
};

inline bool
TokenKindIsBinaryOp(TokenKind tt)
{
    return TOK_BINOP_FIRST <= tt && tt <= TOK_BINOP_LAST;
}

inline bool
TokenKindIsEquality(TokenKind tt)
{
    return TOK_EQUALITY_START <= tt && tt <= TOK_EQUALITY_LAST;
}

inline bool
TokenKindIsRelational(TokenKind tt)
{
    return TOK_RELOP_START <= tt && tt <= TOK_RELOP_LAST;
}

inline bool
TokenKindIsShift(TokenKind tt)
{
    return TOK_SHIFTOP_START <= tt && tt <= TOK_SHIFTOP_LAST;
}

inline bool
TokenKindIsAssignment(TokenKind tt)
{
    return TOK_ASSIGNMENT_START <= tt && tt <= TOK_ASSIGNMENT_LAST;
}

inline bool
TokenKindIsDecl(TokenKind tt)
{
    return tt == TOK_VAR || tt == TOK_LET;
}

} // namespace frontend
} // namespace js

#endif /* frontend_TokenKind_h */
