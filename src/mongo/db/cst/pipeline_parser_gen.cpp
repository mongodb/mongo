// A Bison parser, made by GNU Bison 3.6.3.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2020 Free Software Foundation, Inc.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// As a special exception, you may create a larger work that contains
// part or all of the Bison parser skeleton and distribute that work
// under terms of your choice, so long as that work isn't itself a
// parser generator using the skeleton or a modified version thereof
// as a parser skeleton.  Alternatively, if you modify or redistribute
// the parser skeleton itself, you may (at your option) remove this
// special exception, which will cause the skeleton and the resulting
// Bison output files to be licensed under the GNU General Public
// License without this special exception.

// This special exception was added by the Free Software Foundation in
// version 2.2 of Bison.

// DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
// especially those whose name start with YY_ or yy_.  They are
// private implementation details that can be changed or removed.


#include "pipeline_parser_gen.hpp"


// Unqualified %code blocks.
#line 83 "pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/platform/decimal128.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse,
              str::stream() << msg << " at location " << loc.begin.line << ":" << loc.begin.column
                            << " of input BSON. Lexer produced token of type "
                            << lexer[loc.begin.column].type_get() << ".");
}
}  // namespace mongo

#line 63 "pipeline_parser_gen.cpp"


#ifndef YY_
#if defined YYENABLE_NLS && YYENABLE_NLS
#if ENABLE_NLS
#include <libintl.h>  // FIXME: INFRINGES ON USER NAME SPACE.
#define YY_(msgid) dgettext("bison-runtime", msgid)
#endif
#endif
#ifndef YY_
#define YY_(msgid) msgid
#endif
#endif


// Whether we are compiled with exception support.
#ifndef YY_EXCEPTIONS
#if defined __GNUC__ && !defined __EXCEPTIONS
#define YY_EXCEPTIONS 0
#else
#define YY_EXCEPTIONS 1
#endif
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K].location)
/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
#define YYLLOC_DEFAULT(Current, Rhs, N)                             \
    do                                                              \
        if (N) {                                                    \
            (Current).begin = YYRHSLOC(Rhs, 1).begin;               \
            (Current).end = YYRHSLOC(Rhs, N).end;                   \
        } else {                                                    \
            (Current).begin = (Current).end = YYRHSLOC(Rhs, 0).end; \
        }                                                           \
    while (false)
#endif


// Enable debugging if requested.
#if YYDEBUG

// A pseudo ostream that takes yydebug_ into account.
#define YYCDEBUG  \
    if (yydebug_) \
    (*yycdebug_)

#define YY_SYMBOL_PRINT(Title, Symbol)     \
    do {                                   \
        if (yydebug_) {                    \
            *yycdebug_ << Title << ' ';    \
            yy_print_(*yycdebug_, Symbol); \
            *yycdebug_ << '\n';            \
        }                                  \
    } while (false)

#define YY_REDUCE_PRINT(Rule)       \
    do {                            \
        if (yydebug_)               \
            yy_reduce_print_(Rule); \
    } while (false)

#define YY_STACK_PRINT()       \
    do {                       \
        if (yydebug_)          \
            yy_stack_print_(); \
    } while (false)

#else  // !YYDEBUG

#define YYCDEBUG \
    if (false)   \
    std::cerr
#define YY_SYMBOL_PRINT(Title, Symbol) YYUSE(Symbol)
#define YY_REDUCE_PRINT(Rule) static_cast<void>(0)
#define YY_STACK_PRINT() static_cast<void>(0)

#endif  // !YYDEBUG

#define yyerrok (yyerrstatus_ = 0)
#define yyclearin (yyla.clear())

#define YYACCEPT goto yyacceptlab
#define YYABORT goto yyabortlab
#define YYERROR goto yyerrorlab
#define YYRECOVERING() (!!yyerrstatus_)

#line 58 "pipeline_grammar.yy"
namespace mongo {
#line 156 "pipeline_parser_gen.cpp"

/// Build a parser object.
PipelineParserGen::PipelineParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      lexer(lexer_yyarg),
      cst(cst_yyarg) {
}

PipelineParserGen::~PipelineParserGen() {}

PipelineParserGen::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------------.
| symbol kinds.  |
`---------------*/


// by_state.
PipelineParserGen::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

PipelineParserGen::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void PipelineParserGen::by_state::clear() YY_NOEXCEPT {
    state = empty_state;
}

void PipelineParserGen::by_state::move(by_state& that) {
    state = that.state;
    that.clear();
}

PipelineParserGen::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

PipelineParserGen::symbol_kind_type PipelineParserGen::by_state::kind() const YY_NOEXCEPT {
    if (state == empty_state)
        return symbol_kind::S_YYEMPTY;
    else
        return YY_CAST(symbol_kind_type, yystos_[+state]);
}

PipelineParserGen::stack_symbol_type::stack_symbol_type() {}

PipelineParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 30:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 37:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 39:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 36:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 35:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 38:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 48:  // stageList
        case 49:  // stage
        case 50:  // inhibitOptimization
        case 51:  // unionWith
        case 52:  // num
        case 53:  // skip
        case 54:  // limit
        case 55:  // project
        case 56:  // projectFields
        case 57:  // projection
        case 58:  // compoundExpression
        case 59:  // expressionArray
        case 60:  // expressionObject
        case 61:  // expressionFields
        case 62:  // expression
        case 63:  // maths
        case 64:  // add
        case 65:  // atan2
        case 66:  // string
        case 67:  // binary
        case 68:  // undefined
        case 69:  // objectId
        case 70:  // bool
        case 71:  // date
        case 72:  // null
        case 73:  // regex
        case 74:  // dbPointer
        case 75:  // javascript
        case 76:  // symbol
        case 77:  // javascriptWScope
        case 78:  // int
        case 79:  // timestamp
        case 80:  // long
        case 81:  // double
        case 82:  // decimal
        case 83:  // minKey
        case 84:  // maxKey
        case 85:  // simpleValue
        case 86:  // boolExps
        case 87:  // and
        case 88:  // or
        case 89:  // not
        case 90:  // literalEscapes
        case 91:  // const
        case 92:  // literal
        case 93:  // value
        case 94:  // compoundValue
        case 95:  // valueArray
        case 96:  // valueObject
        case 97:  // valueFields
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 98:   // projectionFieldname
        case 99:   // expressionFieldname
        case 100:  // stageAsUserFieldname
        case 101:  // argAsUserFieldname
        case 102:  // aggExprAsUserFieldname
        case 103:  // invariableUserFieldname
        case 104:  // idAsUserFieldname
        case 105:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 33:  // DATE
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 44:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 32:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 41:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 46:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 45:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 34:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 31:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 43:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 40:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 42:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 106:  // projectField
        case 107:  // expressionField
        case 108:  // valueField
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 28:  // FIELDNAME
        case 29:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 109:  // expressions
        case 110:  // values
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
}

PipelineParserGen::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
    : super_type(s, YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 30:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 37:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 39:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 36:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 35:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 38:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 48:  // stageList
        case 49:  // stage
        case 50:  // inhibitOptimization
        case 51:  // unionWith
        case 52:  // num
        case 53:  // skip
        case 54:  // limit
        case 55:  // project
        case 56:  // projectFields
        case 57:  // projection
        case 58:  // compoundExpression
        case 59:  // expressionArray
        case 60:  // expressionObject
        case 61:  // expressionFields
        case 62:  // expression
        case 63:  // maths
        case 64:  // add
        case 65:  // atan2
        case 66:  // string
        case 67:  // binary
        case 68:  // undefined
        case 69:  // objectId
        case 70:  // bool
        case 71:  // date
        case 72:  // null
        case 73:  // regex
        case 74:  // dbPointer
        case 75:  // javascript
        case 76:  // symbol
        case 77:  // javascriptWScope
        case 78:  // int
        case 79:  // timestamp
        case 80:  // long
        case 81:  // double
        case 82:  // decimal
        case 83:  // minKey
        case 84:  // maxKey
        case 85:  // simpleValue
        case 86:  // boolExps
        case 87:  // and
        case 88:  // or
        case 89:  // not
        case 90:  // literalEscapes
        case 91:  // const
        case 92:  // literal
        case 93:  // value
        case 94:  // compoundValue
        case 95:  // valueArray
        case 96:  // valueObject
        case 97:  // valueFields
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 98:   // projectionFieldname
        case 99:   // expressionFieldname
        case 100:  // stageAsUserFieldname
        case 101:  // argAsUserFieldname
        case 102:  // aggExprAsUserFieldname
        case 103:  // invariableUserFieldname
        case 104:  // idAsUserFieldname
        case 105:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 33:  // DATE
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 44:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 32:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 41:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 46:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 45:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 34:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 31:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 43:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 40:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 42:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 106:  // projectField
        case 107:  // expressionField
        case 108:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 28:  // FIELDNAME
        case 29:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 109:  // expressions
        case 110:  // values
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
}

#if YY_CPLUSPLUS < 201103L
PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 30:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 37:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 39:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 36:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 35:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 38:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 48:  // stageList
        case 49:  // stage
        case 50:  // inhibitOptimization
        case 51:  // unionWith
        case 52:  // num
        case 53:  // skip
        case 54:  // limit
        case 55:  // project
        case 56:  // projectFields
        case 57:  // projection
        case 58:  // compoundExpression
        case 59:  // expressionArray
        case 60:  // expressionObject
        case 61:  // expressionFields
        case 62:  // expression
        case 63:  // maths
        case 64:  // add
        case 65:  // atan2
        case 66:  // string
        case 67:  // binary
        case 68:  // undefined
        case 69:  // objectId
        case 70:  // bool
        case 71:  // date
        case 72:  // null
        case 73:  // regex
        case 74:  // dbPointer
        case 75:  // javascript
        case 76:  // symbol
        case 77:  // javascriptWScope
        case 78:  // int
        case 79:  // timestamp
        case 80:  // long
        case 81:  // double
        case 82:  // decimal
        case 83:  // minKey
        case 84:  // maxKey
        case 85:  // simpleValue
        case 86:  // boolExps
        case 87:  // and
        case 88:  // or
        case 89:  // not
        case 90:  // literalEscapes
        case 91:  // const
        case 92:  // literal
        case 93:  // value
        case 94:  // compoundValue
        case 95:  // valueArray
        case 96:  // valueObject
        case 97:  // valueFields
            value.copy<CNode>(that.value);
            break;

        case 98:   // projectionFieldname
        case 99:   // expressionFieldname
        case 100:  // stageAsUserFieldname
        case 101:  // argAsUserFieldname
        case 102:  // aggExprAsUserFieldname
        case 103:  // invariableUserFieldname
        case 104:  // idAsUserFieldname
        case 105:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 33:  // DATE
            value.copy<Date_t>(that.value);
            break;

        case 44:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 32:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 41:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 46:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 45:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 34:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 31:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 43:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 40:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 42:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 106:  // projectField
        case 107:  // expressionField
        case 108:  // valueField
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 28:  // FIELDNAME
        case 29:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 109:  // expressions
        case 110:  // values
            value.copy<std::vector<CNode>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    return *this;
}

PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 30:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 37:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 39:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 36:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 35:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 38:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 48:  // stageList
        case 49:  // stage
        case 50:  // inhibitOptimization
        case 51:  // unionWith
        case 52:  // num
        case 53:  // skip
        case 54:  // limit
        case 55:  // project
        case 56:  // projectFields
        case 57:  // projection
        case 58:  // compoundExpression
        case 59:  // expressionArray
        case 60:  // expressionObject
        case 61:  // expressionFields
        case 62:  // expression
        case 63:  // maths
        case 64:  // add
        case 65:  // atan2
        case 66:  // string
        case 67:  // binary
        case 68:  // undefined
        case 69:  // objectId
        case 70:  // bool
        case 71:  // date
        case 72:  // null
        case 73:  // regex
        case 74:  // dbPointer
        case 75:  // javascript
        case 76:  // symbol
        case 77:  // javascriptWScope
        case 78:  // int
        case 79:  // timestamp
        case 80:  // long
        case 81:  // double
        case 82:  // decimal
        case 83:  // minKey
        case 84:  // maxKey
        case 85:  // simpleValue
        case 86:  // boolExps
        case 87:  // and
        case 88:  // or
        case 89:  // not
        case 90:  // literalEscapes
        case 91:  // const
        case 92:  // literal
        case 93:  // value
        case 94:  // compoundValue
        case 95:  // valueArray
        case 96:  // valueObject
        case 97:  // valueFields
            value.move<CNode>(that.value);
            break;

        case 98:   // projectionFieldname
        case 99:   // expressionFieldname
        case 100:  // stageAsUserFieldname
        case 101:  // argAsUserFieldname
        case 102:  // aggExprAsUserFieldname
        case 103:  // invariableUserFieldname
        case 104:  // idAsUserFieldname
        case 105:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 33:  // DATE
            value.move<Date_t>(that.value);
            break;

        case 44:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 32:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 41:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 46:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 45:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 34:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 31:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 43:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 40:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 42:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 106:  // projectField
        case 107:  // expressionField
        case 108:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 28:  // FIELDNAME
        case 29:  // STRING
            value.move<std::string>(that.value);
            break;

        case 109:  // expressions
        case 110:  // values
            value.move<std::vector<CNode>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    // that is emptied.
    that.state = empty_state;
    return *this;
}
#endif

template <typename Base>
void PipelineParserGen::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
    if (yymsg)
        YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void PipelineParserGen::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
    std::ostream& yyoutput = yyo;
    YYUSE(yyoutput);
    if (yysym.empty())
        yyo << "empty symbol";
    else {
        symbol_kind_type yykind = yysym.kind();
        yyo << (yykind < YYNTOKENS ? "token" : "nterm") << ' ' << yysym.name() << " ("
            << yysym.location << ": ";
        YYUSE(yykind);
        yyo << ')';
    }
}
#endif

void PipelineParserGen::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
    if (m)
        YY_SYMBOL_PRINT(m, sym);
    yystack_.push(YY_MOVE(sym));
}

void PipelineParserGen::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
    yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
    stack_symbol_type ss(s, sym);
    yypush_(m, ss);
#endif
}

void PipelineParserGen::yypop_(int n) {
    yystack_.pop(n);
}

#if YYDEBUG
std::ostream& PipelineParserGen::debug_stream() const {
    return *yycdebug_;
}

void PipelineParserGen::set_debug_stream(std::ostream& o) {
    yycdebug_ = &o;
}


PipelineParserGen::debug_level_type PipelineParserGen::debug_level() const {
    return yydebug_;
}

void PipelineParserGen::set_debug_level(debug_level_type l) {
    yydebug_ = l;
}
#endif  // YYDEBUG

PipelineParserGen::state_type PipelineParserGen::yy_lr_goto_state_(state_type yystate, int yysym) {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - YYNTOKENS];
}

bool PipelineParserGen::yy_pact_value_is_default_(int yyvalue) {
    return yyvalue == yypact_ninf_;
}

bool PipelineParserGen::yy_table_value_is_error_(int yyvalue) {
    return yyvalue == yytable_ninf_;
}

int PipelineParserGen::operator()() {
    return parse();
}

int PipelineParserGen::parse() {
    int yyn;
    /// Length of the RHS of the rule being reduced.
    int yylen = 0;

    // Error handling.
    int yynerrs_ = 0;
    int yyerrstatus_ = 0;

    /// The lookahead symbol.
    symbol_type yyla;

    /// The locations where the error started and ended.
    stack_symbol_type yyerror_range[3];

    /// The return value of parse ().
    int yyresult;

#if YY_EXCEPTIONS
    try
#endif  // YY_EXCEPTIONS
    {
        YYCDEBUG << "Starting parse\n";


        /* Initialize the stack.  The initial state will be set in
           yynewstate, since the latter expects the semantical and the
           location values to have been already stored, initialize these
           stacks with a primary value.  */
        yystack_.clear();
        yypush_(YY_NULLPTR, 0, YY_MOVE(yyla));

    /*-----------------------------------------------.
    | yynewstate -- push a new symbol on the stack.  |
    `-----------------------------------------------*/
    yynewstate:
        YYCDEBUG << "Entering state " << int(yystack_[0].state) << '\n';
        YY_STACK_PRINT();

        // Accept?
        if (yystack_[0].state == yyfinal_)
            YYACCEPT;

        goto yybackup;


    /*-----------.
    | yybackup.  |
    `-----------*/
    yybackup:
        // Try to take a decision without lookahead.
        yyn = yypact_[+yystack_[0].state];
        if (yy_pact_value_is_default_(yyn))
            goto yydefault;

        // Read a lookahead token.
        if (yyla.empty()) {
            YYCDEBUG << "Reading a token\n";
#if YY_EXCEPTIONS
            try
#endif  // YY_EXCEPTIONS
            {
                symbol_type yylookahead(yylex(lexer));
                yyla.move(yylookahead);
            }
#if YY_EXCEPTIONS
            catch (const syntax_error& yyexc) {
                YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
                error(yyexc);
                goto yyerrlab1;
            }
#endif  // YY_EXCEPTIONS
        }
        YY_SYMBOL_PRINT("Next token is", yyla);

        if (yyla.kind() == symbol_kind::S_YYerror) {
            // The scanner already issued an error message, process directly
            // to error recovery.  But do not keep the error token as
            // lookahead, it is too special and may lead us to an endless
            // loop in error recovery. */
            yyla.kind_ = symbol_kind::S_YYUNDEF;
            goto yyerrlab1;
        }

        /* If the proper action on seeing token YYLA.TYPE is to reduce or
           to detect an error, take that action.  */
        yyn += yyla.kind();
        if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.kind()) {
            goto yydefault;
        }

        // Reduce or error.
        yyn = yytable_[yyn];
        if (yyn <= 0) {
            if (yy_table_value_is_error_(yyn))
                goto yyerrlab;
            yyn = -yyn;
            goto yyreduce;
        }

        // Count tokens shifted since error; after three, turn off error status.
        if (yyerrstatus_)
            --yyerrstatus_;

        // Shift the lookahead token.
        yypush_("Shifting", state_type(yyn), YY_MOVE(yyla));
        goto yynewstate;


    /*-----------------------------------------------------------.
    | yydefault -- do the default action for the current state.  |
    `-----------------------------------------------------------*/
    yydefault:
        yyn = yydefact_[+yystack_[0].state];
        if (yyn == 0)
            goto yyerrlab;
        goto yyreduce;


    /*-----------------------------.
    | yyreduce -- do a reduction.  |
    `-----------------------------*/
    yyreduce:
        yylen = yyr2_[yyn];
        {
            stack_symbol_type yylhs;
            yylhs.state = yy_lr_goto_state_(yystack_[yylen].state, yyr1_[yyn]);
            /* Variants are always initialized to an empty instance of the
               correct type. The default '$$ = $1' action is NOT applied
               when using variants.  */
            switch (yyr1_[yyn]) {
                case 30:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 37:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 39:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 36:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 35:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 38:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 48:  // stageList
                case 49:  // stage
                case 50:  // inhibitOptimization
                case 51:  // unionWith
                case 52:  // num
                case 53:  // skip
                case 54:  // limit
                case 55:  // project
                case 56:  // projectFields
                case 57:  // projection
                case 58:  // compoundExpression
                case 59:  // expressionArray
                case 60:  // expressionObject
                case 61:  // expressionFields
                case 62:  // expression
                case 63:  // maths
                case 64:  // add
                case 65:  // atan2
                case 66:  // string
                case 67:  // binary
                case 68:  // undefined
                case 69:  // objectId
                case 70:  // bool
                case 71:  // date
                case 72:  // null
                case 73:  // regex
                case 74:  // dbPointer
                case 75:  // javascript
                case 76:  // symbol
                case 77:  // javascriptWScope
                case 78:  // int
                case 79:  // timestamp
                case 80:  // long
                case 81:  // double
                case 82:  // decimal
                case 83:  // minKey
                case 84:  // maxKey
                case 85:  // simpleValue
                case 86:  // boolExps
                case 87:  // and
                case 88:  // or
                case 89:  // not
                case 90:  // literalEscapes
                case 91:  // const
                case 92:  // literal
                case 93:  // value
                case 94:  // compoundValue
                case 95:  // valueArray
                case 96:  // valueObject
                case 97:  // valueFields
                    yylhs.value.emplace<CNode>();
                    break;

                case 98:   // projectionFieldname
                case 99:   // expressionFieldname
                case 100:  // stageAsUserFieldname
                case 101:  // argAsUserFieldname
                case 102:  // aggExprAsUserFieldname
                case 103:  // invariableUserFieldname
                case 104:  // idAsUserFieldname
                case 105:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 33:  // DATE
                    yylhs.value.emplace<Date_t>();
                    break;

                case 44:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 32:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 41:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 46:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 45:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 34:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 31:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 43:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 40:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 42:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 106:  // projectField
                case 107:  // expressionField
                case 108:  // valueField
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 28:  // FIELDNAME
                case 29:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 109:  // expressions
                case 110:  // values
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                default:
                    break;
            }


            // Default location.
            {
                stack_type::slice range(yystack_, yylen);
                YYLLOC_DEFAULT(yylhs.location, range, yylen);
                yyerror_range[1].location = yylhs.location;
            }

            // Perform the reduction.
            YY_REDUCE_PRINT(yyn);
#if YY_EXCEPTIONS
            try
#endif  // YY_EXCEPTIONS
            {
                switch (yyn) {
                    case 2:
#line 191 "pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1293 "pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 197 "pipeline_grammar.yy"
                    {
                    }
#line 1299 "pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 198 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1307 "pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 206 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1313 "pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1319 "pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1325 "pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1331 "pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1337 "pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1343 "pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 213 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1351 "pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 219 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1364 "pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 229 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1370 "pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 229 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1376 "pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 229 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1382 "pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 229 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1388 "pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 233 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1396 "pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 238 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1404 "pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 243 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1412 "pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 249 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1420 "pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 252 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1429 "pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 259 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1437 "pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 262 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1445 "pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 268 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1451 "pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 269 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1459 "pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 272 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1467 "pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1475 "pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 278 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1483 "pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 281 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1491 "pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 284 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1499 "pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 287 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1507 "pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 290 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1515 "pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 293 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1523 "pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 296 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1531 "pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 299 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1537 "pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 303 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1543 "pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 303 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1549 "pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 303 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1555 "pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 303 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1561 "pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1569 "pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 315 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1577 "pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 318 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1585 "pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 321 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1593 "pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 324 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1601 "pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 327 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1609 "pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1617 "pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 339 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1625 "pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 347 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1633 "pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 350 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1641 "pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 353 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1649 "pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 356 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 1657 "pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 1665 "pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 362 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1673 "pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 365 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1681 "pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 371 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 1689 "pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 377 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 1697 "pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 383 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 1705 "pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 389 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 1713 "pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 395 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 1721 "pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 1729 "pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 407 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 1737 "pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 413 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 1745 "pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 419 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 1753 "pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 425 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 1761 "pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 431 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 1769 "pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 437 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 1777 "pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 443 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 1785 "pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 449 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 1793 "pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 455 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1801 "pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 458 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0}};
                    }
#line 1809 "pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 464 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1817 "pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 467 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 1825 "pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 473 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1833 "pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 476 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 1841 "pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 482 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1849 "pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 1857 "pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 491 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 1865 "pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 494 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 1873 "pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 500 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1879 "pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 501 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1885 "pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 502 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1891 "pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 503 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1897 "pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 504 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1903 "pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 505 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1909 "pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 506 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1915 "pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 507 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1921 "pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 508 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1927 "pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 509 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1933 "pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 510 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1939 "pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 511 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1945 "pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 512 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1951 "pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 513 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1957 "pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 514 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1963 "pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 515 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1969 "pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 516 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1975 "pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 517 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1981 "pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 518 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1987 "pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 525 "pipeline_grammar.yy"
                    {
                    }
#line 1993 "pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 526 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2002 "pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 533 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2008 "pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 533 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2014 "pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2020 "pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2026 "pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2032 "pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2038 "pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2044 "pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 543 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2052 "pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 551 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2060 "pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 557 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2068 "pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 560 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2077 "pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 567 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2085 "pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2091 "pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2097 "pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2103 "pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2109 "pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 578 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2117 "pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 584 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2123 "pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 585 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2129 "pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 589 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2141 "pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 599 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2150 "pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 606 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2156 "pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 606 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2162 "pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 606 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2168 "pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 610 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2180 "pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 620 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 2192 "pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 630 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2201 "pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 637 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2207 "pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 637 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2213 "pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 641 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2222 "pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 648 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2231 "pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 655 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2237 "pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 655 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2243 "pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 659 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2249 "pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 659 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2255 "pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 663 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2263 "pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 669 "pipeline_grammar.yy"
                    {
                    }
#line 2269 "pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 670 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2278 "pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 677 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2286 "pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 683 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2294 "pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 686 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2303 "pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 693 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2311 "pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 700 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2317 "pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 701 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2323 "pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 702 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2329 "pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 703 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2335 "pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 704 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2341 "pipeline_parser_gen.cpp"
                    break;


#line 2345 "pipeline_parser_gen.cpp"

                    default:
                        break;
                }
            }
#if YY_EXCEPTIONS
            catch (const syntax_error& yyexc) {
                YYCDEBUG << "Caught exception: " << yyexc.what() << '\n';
                error(yyexc);
                YYERROR;
            }
#endif  // YY_EXCEPTIONS
            YY_SYMBOL_PRINT("-> $$ =", yylhs);
            yypop_(yylen);
            yylen = 0;

            // Shift the result of the reduction.
            yypush_(YY_NULLPTR, YY_MOVE(yylhs));
        }
        goto yynewstate;


    /*--------------------------------------.
    | yyerrlab -- here on detecting error.  |
    `--------------------------------------*/
    yyerrlab:
        // If not already recovering from an error, report this error.
        if (!yyerrstatus_) {
            ++yynerrs_;
            std::string msg = YY_("syntax error");
            error(yyla.location, YY_MOVE(msg));
        }


        yyerror_range[1].location = yyla.location;
        if (yyerrstatus_ == 3) {
            /* If just tried and failed to reuse lookahead token after an
               error, discard it.  */

            // Return failure if at end of input.
            if (yyla.kind() == symbol_kind::S_YYEOF)
                YYABORT;
            else if (!yyla.empty()) {
                yy_destroy_("Error: discarding", yyla);
                yyla.clear();
            }
        }

        // Else will try to reuse lookahead token after shifting the error token.
        goto yyerrlab1;


    /*---------------------------------------------------.
    | yyerrorlab -- error raised explicitly by YYERROR.  |
    `---------------------------------------------------*/
    yyerrorlab:
        /* Pacify compilers when the user code never invokes YYERROR and
           the label yyerrorlab therefore never appears in user code.  */
        if (false)
            YYERROR;

        /* Do not reclaim the symbols of the rule whose action triggered
           this YYERROR.  */
        yypop_(yylen);
        yylen = 0;
        YY_STACK_PRINT();
        goto yyerrlab1;


    /*-------------------------------------------------------------.
    | yyerrlab1 -- common code for both syntax error and YYERROR.  |
    `-------------------------------------------------------------*/
    yyerrlab1:
        yyerrstatus_ = 3;  // Each real token shifted decrements this.
        // Pop stack until we find a state that shifts the error token.
        for (;;) {
            yyn = yypact_[+yystack_[0].state];
            if (!yy_pact_value_is_default_(yyn)) {
                yyn += symbol_kind::S_YYerror;
                if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == symbol_kind::S_YYerror) {
                    yyn = yytable_[yyn];
                    if (0 < yyn)
                        break;
                }
            }

            // Pop the current state because it cannot handle the error token.
            if (yystack_.size() == 1)
                YYABORT;

            yyerror_range[1].location = yystack_[0].location;
            yy_destroy_("Error: popping", yystack_[0]);
            yypop_();
            YY_STACK_PRINT();
        }
        {
            stack_symbol_type error_token;

            yyerror_range[2].location = yyla.location;
            YYLLOC_DEFAULT(error_token.location, yyerror_range, 2);

            // Shift the error token.
            error_token.state = state_type(yyn);
            yypush_("Shifting", YY_MOVE(error_token));
        }
        goto yynewstate;


    /*-------------------------------------.
    | yyacceptlab -- YYACCEPT comes here.  |
    `-------------------------------------*/
    yyacceptlab:
        yyresult = 0;
        goto yyreturn;


    /*-----------------------------------.
    | yyabortlab -- YYABORT comes here.  |
    `-----------------------------------*/
    yyabortlab:
        yyresult = 1;
        goto yyreturn;


    /*-----------------------------------------------------.
    | yyreturn -- parsing is finished, return the result.  |
    `-----------------------------------------------------*/
    yyreturn:
        if (!yyla.empty())
            yy_destroy_("Cleanup: discarding lookahead", yyla);

        /* Do not reclaim the symbols of the rule whose action triggered
           this YYABORT or YYACCEPT.  */
        yypop_(yylen);
        YY_STACK_PRINT();
        while (1 < yystack_.size()) {
            yy_destroy_("Cleanup: popping", yystack_[0]);
            yypop_();
        }

        return yyresult;
    }
#if YY_EXCEPTIONS
    catch (...) {
        YYCDEBUG << "Exception caught: cleaning lookahead and stack\n";
        // Do not try to display the values of the reclaimed symbols,
        // as their printers might throw an exception.
        if (!yyla.empty())
            yy_destroy_(YY_NULLPTR, yyla);

        while (1 < yystack_.size()) {
            yy_destroy_(YY_NULLPTR, yystack_[0]);
            yypop_();
        }
        throw;
    }
#endif  // YY_EXCEPTIONS
}

void PipelineParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

#if YYDEBUG || 0
const char* PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytname_[yysymbol];
}
#endif  // #if YYDEBUG || 0


const short PipelineParserGen::yypact_ninf_ = -142;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    30,   36,   40,   16,   35,   -142, 39,   -142, 110,  110,  42,   44,   -142, -142, -142,
    -142, -142, -142, 48,   24,   43,   -142, -142, -142, -142, -142, -142, -142, -142, -142,
    -142, -142, -142, -142, -142, -142, 36,   -142, 25,   -142, 171,  -142, -142, 38,   -142,
    7,    -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142,
    -142, 7,    -142, -142, -142, -142, -142, 1,    56,   59,   -142, -142, -142, -142, -142,
    -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142,
    -142, -142, -142, -142, -142, -142, 52,   55,   61,   68,   79,   80,   81,   82,   221,
    -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142,
    -142, 59,   -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142, -142,
    -142, -142, -142, -142, -142, -142, -142, 101,  -142, 59,   59,   59,   103,  103,  59,
    59,   -142, -142, 59,   -142, -142, -142, -142, -142, -142, -142, 59,   59,   59,   -142,
    103,  -142, 104,  -142, -142, -142, 111,  59,   116,  -142, 59,   117,  59,   196,  103,
    118,  57,   105,  59,   121,  120,  123,  122,  -142, -142, -142, -142, -142, -142, 103,
    -142, -142, -142, -142, -142, 124,  -142, 125,  -142, 127,  -142, 147,  -142, -142, -142};

const unsigned char PipelineParserGen::yydefact_[] = {
    0,   3,   0,   0,   0,   1,   0,   5,   0,   0,   0,   0,   7,   8,   9,   10,  11,  2,
    0,   0,   0,   71,  73,  75,  77,  70,  72,  74,  76,  18,  14,  15,  16,  17,  19,  21,
    3,   12,  0,   6,   0,   4,   56,  0,   20,  0,   42,  43,  44,  45,  46,  47,  48,  49,
    50,  51,  52,  53,  54,  55,  41,  0,   38,  39,  40,  37,  22,  0,   110, 99,  27,  29,
    31,  33,  34,  35,  26,  28,  30,  32,  23,  36,  103, 104, 105, 118, 119, 25,  106, 122,
    123, 124, 107, 128, 129, 24,  0,   0,   0,   0,   0,   0,   0,   0,   0,   78,  79,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  102, 99,  80,  81,  82,  83,
    95,  84,  85,  86,  87,  88,  89,  90,  91,  96,  92,  93,  94,  97,  98,  101, 0,   13,
    0,   0,   0,   0,   0,   0,   0,   109, 117, 0,   114, 115, 113, 116, 111, 100, 108, 0,
    0,   0,   140, 137, 132, 0,   133, 134, 135, 0,   0,   0,   112, 99,  0,   99,  0,   137,
    0,   0,   0,   99,  0,   0,   0,   0,   139, 144, 145, 146, 143, 147, 0,   141, 138, 136,
    130, 131, 0,   127, 0,   121, 0,   142, 0,   120, 125, 126};

const short PipelineParserGen::yypgoto_[] = {
    -142, 126,  -142, -142, -142, 149,  -142, -142, -142, -142, 98,   -32,  -142, -142,
    -142, 11,   -142, -142, -142, -24,  -142, -142, -142, -142, -142, -142, -142, -142,
    -142, -142, -142, 14,   -142, 17,   -4,   19,   -142, -142, -141, -142, -142, -142,
    -142, -142, -142, -142, -139, -142, -142, -142, -142, -142, -142, -104, -103, -15,
    -102, -13,  -142, -142, -142, -142, -118, -12,  -142, -142, -142};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  4,   11,  12,  13,  29,  14,  15,  16,  40,  80,  120, 82,  83,  104, 121, 84,
    85,  86,  122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136,
    137, 138, 139, 140, 141, 88,  89,  90,  91,  92,  93,  94,  179, 168, 169, 170, 178,
    61,  153, 62,  63,  64,  65,  157, 194, 66,  158, 195, 142, 180, 2,   19,  20};

const unsigned char PipelineParserGen::yytable_[] = {
    154, 155, 156, 159, 32,  32,  166, 166, 167, 171, 68,  23,  69,  81,  43,  70,  71,  72,
    73,  74,  75,  87,  30,  30,  166, 31,  31,  33,  33,  81,  6,   7,   8,   9,   10,  1,
    42,  87,  166, 3,   5,   17,  18,  38,  27,  35,  39,  76,  36,  77,  78,  79,  37,  166,
    42,  205, 143, 185, 67,  187, 144, 198, 68,  96,  69,  200, 145, 21,  22,  23,  24,  105,
    106, 146, 189, 190, 192, 97,  98,  99,  100, 101, 102, 103, 147, 148, 149, 150, 42,  107,
    108, 109, 110, 111, 112, 113, 114, 115, 116, 25,  117, 26,  27,  28,  118, 119, 164, 160,
    165, 199, 181, 21,  22,  23,  24,  105, 106, 182, 21,  22,  23,  24,  184, 186, 197, 201,
    202, 203, 204, 207, 206, 208, 42,  107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 25,
    117, 26,  27,  28,  118, 119, 25,  209, 26,  27,  28,  161, 162, 163, 34,  95,  172, 173,
    41,  191, 174, 193, 0,   196, 0,   0,   0,   0,   175, 176, 177, 44,  0,   0,   45,  0,
    0,   0,   0,   183, 0,   46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  188, 0,   0,   152, 0,   0,   0,   0,   0,   0,   46,  47,  48,  49,  50,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  151, 0,   0,   152, 0,   0,   0,   0,   0,
    0,   46,  47,  48,  49,  50,  51,  52,  0,   0,   0,   0,   0,   0,   0,   60};

const short PipelineParserGen::yycheck_[] = {
    104, 104, 104, 121, 8,   9,   147, 148, 147, 148, 3,   10,  5,   45,  38, 8,  9,   10,
    11,  12,  13,  45,  8,   9,   165, 8,   9,   8,   9,   61,  14,  15,  16, 17, 18,  5,
    29,  61,  179, 3,   0,   6,   3,   19,  43,  3,   3,   40,  4,   42,  43, 44, 4,   194,
    29,  194, 4,   175, 20,  177, 5,   4,   3,   67,  5,   183, 5,   8,   9,  10, 11,  12,
    13,  5,   178, 178, 178, 21,  22,  23,  24,  25,  26,  27,  5,   5,   5,  5,  29,  30,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45, 46, 3,   6,
    5,   4,   6,   8,   9,   10,  11,  12,  13,  6,   8,   9,   10,  11,  6,  6,  6,   4,
    6,   4,   6,   4,   6,   4,   29,  30,  31,  32,  33,  34,  35,  36,  37, 38, 39,  40,
    41,  42,  43,  44,  45,  46,  40,  4,   42,  43,  44,  144, 145, 146, 9,  61, 149, 150,
    36,  178, 153, 178, -1,  179, -1,  -1,  -1,  -1,  161, 162, 163, 4,   -1, -1, 7,   -1,
    -1,  -1,  -1,  172, -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23, 24, 25,  26,
    27,  28,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16, 17, 18,  19,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  4,   -1,  -1,  7,   -1,  -1, -1, -1,  -1,
    -1,  14,  15,  16,  17,  18,  19,  20,  -1,  -1,  -1,  -1,  -1,  -1,  -1, 28};

const signed char PipelineParserGen::yystos_[] = {
    0,   5,   111, 3,  48, 0,   14, 15,  16,  17,  18,  49,  50,  51,  53,  54,  55,  6,
    3,   112, 113, 8,  9,  10,  11, 40,  42,  43,  44,  52,  78,  80,  81,  82,  52,  3,
    4,   4,   19,  3,  56, 48,  29, 66,  4,   7,   14,  15,  16,  17,  18,  19,  20,  21,
    22,  23,  24,  25, 26, 27,  28, 98,  100, 101, 102, 103, 106, 20,  3,   5,   8,   9,
    10,  11,  12,  13, 40, 42,  43, 44,  57,  58,  59,  60,  63,  64,  65,  66,  86,  87,
    88,  89,  90,  91, 92, 57,  81, 21,  22,  23,  24,  25,  26,  27,  61,  12,  13,  30,
    31,  32,  33,  34, 35, 36,  37, 38,  39,  41,  45,  46,  58,  62,  66,  67,  68,  69,
    70,  71,  72,  73, 74, 75,  76, 77,  78,  79,  80,  81,  82,  83,  84,  85,  109, 4,
    5,   5,   5,   5,  5,  5,   5,  4,   7,   99,  100, 101, 103, 104, 107, 109, 6,   62,
    62,  62,  3,   5,  85, 93,  94, 95,  96,  93,  62,  62,  62,  62,  62,  62,  97,  93,
    110, 6,   6,   62, 6,  109, 6,  109, 4,   100, 101, 102, 103, 104, 105, 108, 110, 6,
    4,   4,   109, 4,  6,  4,   6,  93,  6,   4,   4,   4};

const signed char PipelineParserGen::yyr1_[] = {
    0,  47, 111, 48,  48,  113, 112, 49,  49,  49,  49,  49,  50,  51,  52,  52,  52,  52,  53,
    54, 55, 56,  56,  106, 106, 57,  57,  57,  57,  57,  57,  57,  57,  57,  57,  57,  57,  98,
    98, 98, 98,  103, 100, 100, 100, 100, 100, 101, 101, 102, 102, 102, 102, 102, 102, 102, 66,
    67, 68, 69,  71,  72,  73,  74,  75,  76,  77,  79,  83,  84,  78,  78,  80,  80,  81,  81,
    82, 82, 70,  70,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,  85,
    85, 85, 85,  85,  109, 109, 62,  62,  58,  58,  58,  58,  58,  59,  60,  61,  61,  107, 99,
    99, 99, 99,  104, 63,  63,  64,  65,  86,  86,  86,  87,  88,  89,  90,  90,  91,  92,  93,
    93, 94, 94,  95,  110, 110, 96,  97,  97,  108, 105, 105, 105, 105, 105};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    8, 7, 1, 1, 1, 8, 8, 6, 1, 1, 6, 6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1};


#if YYDEBUG
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "\"invalid token\"",
                                                   "START_OBJECT",
                                                   "END_OBJECT",
                                                   "START_ARRAY",
                                                   "END_ARRAY",
                                                   "ID",
                                                   "INT_ZERO",
                                                   "LONG_ZERO",
                                                   "DOUBLE_ZERO",
                                                   "DECIMAL_ZERO",
                                                   "BOOL_TRUE",
                                                   "BOOL_FALSE",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_UNION_WITH",
                                                   "STAGE_SKIP",
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "ADD",
                                                   "ATAN2",
                                                   "AND",
                                                   "CONST",
                                                   "LITERAL",
                                                   "OR",
                                                   "NOT",
                                                   "FIELDNAME",
                                                   "STRING",
                                                   "BINARY",
                                                   "UNDEFINED",
                                                   "OBJECT_ID",
                                                   "DATE",
                                                   "JSNULL",
                                                   "REGEX",
                                                   "DB_POINTER",
                                                   "JAVASCRIPT",
                                                   "SYMBOL",
                                                   "JAVASCRIPT_W_SCOPE",
                                                   "INT_NON_ZERO",
                                                   "TIMESTAMP",
                                                   "LONG_NON_ZERO",
                                                   "DOUBLE_NON_ZERO",
                                                   "DECIMAL_NON_ZERO",
                                                   "MIN_KEY",
                                                   "MAX_KEY",
                                                   "$accept",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "num",
                                                   "skip",
                                                   "limit",
                                                   "project",
                                                   "projectFields",
                                                   "projection",
                                                   "compoundExpression",
                                                   "expressionArray",
                                                   "expressionObject",
                                                   "expressionFields",
                                                   "expression",
                                                   "maths",
                                                   "add",
                                                   "atan2",
                                                   "string",
                                                   "binary",
                                                   "undefined",
                                                   "objectId",
                                                   "bool",
                                                   "date",
                                                   "null",
                                                   "regex",
                                                   "dbPointer",
                                                   "javascript",
                                                   "symbol",
                                                   "javascriptWScope",
                                                   "int",
                                                   "timestamp",
                                                   "long",
                                                   "double",
                                                   "decimal",
                                                   "minKey",
                                                   "maxKey",
                                                   "simpleValue",
                                                   "boolExps",
                                                   "and",
                                                   "or",
                                                   "not",
                                                   "literalEscapes",
                                                   "const",
                                                   "literal",
                                                   "value",
                                                   "compoundValue",
                                                   "valueArray",
                                                   "valueObject",
                                                   "valueFields",
                                                   "projectionFieldname",
                                                   "expressionFieldname",
                                                   "stageAsUserFieldname",
                                                   "argAsUserFieldname",
                                                   "aggExprAsUserFieldname",
                                                   "invariableUserFieldname",
                                                   "idAsUserFieldname",
                                                   "valueFieldname",
                                                   "projectField",
                                                   "expressionField",
                                                   "valueField",
                                                   "expressions",
                                                   "values",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,   191, 191, 197, 198, 206, 206, 209, 209, 209, 209, 209, 213, 219, 229, 229, 229, 229, 233,
    238, 243, 249, 252, 259, 262, 268, 269, 272, 275, 278, 281, 284, 287, 290, 293, 296, 299, 303,
    303, 303, 303, 307, 315, 318, 321, 324, 327, 336, 339, 347, 350, 353, 356, 359, 362, 365, 371,
    377, 383, 389, 395, 401, 407, 413, 419, 425, 431, 437, 443, 449, 455, 458, 464, 467, 473, 476,
    482, 485, 491, 494, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514,
    515, 516, 517, 518, 525, 526, 533, 533, 537, 537, 537, 537, 537, 543, 551, 557, 560, 567, 574,
    574, 574, 574, 578, 584, 585, 589, 599, 606, 606, 606, 610, 620, 630, 637, 637, 641, 648, 655,
    655, 659, 659, 663, 669, 670, 677, 683, 686, 693, 700, 701, 702, 703, 704};

void PipelineParserGen::yy_stack_print_() const {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

void PipelineParserGen::yy_reduce_print_(int yyrule) const {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 58 "pipeline_grammar.yy"
}  // namespace mongo
#line 2824 "pipeline_parser_gen.cpp"

#line 707 "pipeline_grammar.yy"
