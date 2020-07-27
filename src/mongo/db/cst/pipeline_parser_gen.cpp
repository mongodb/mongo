// A Bison parser, made by GNU Bison 3.5.4.

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

// Undocumented macros, especially those whose name start with YY_,
// are private implementation details.  Do not rely on them.


#include "pipeline_parser_gen.hpp"


// Unqualified %code blocks.
#line 83 "src/mongo/db/cst/pipeline_grammar.yy"

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

#line 62 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#define YY_STACK_PRINT()      \
    do {                      \
        if (yydebug_)         \
            yystack_print_(); \
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

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 154 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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
| Symbol types.  |
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

PipelineParserGen::symbol_number_type PipelineParserGen::by_state::type_get() const YY_NOEXCEPT {
    if (state == empty_state)
        return empty_symbol;
    else
        return yystos_[+state];
}

PipelineParserGen::stack_symbol_type::stack_symbol_type() {}

PipelineParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.type_get()) {
        case 37:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 44:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 46:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 43:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 42:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 45:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 55:   // stageList
        case 56:   // stage
        case 57:   // inhibitOptimization
        case 58:   // unionWith
        case 59:   // num
        case 60:   // skip
        case 61:   // limit
        case 62:   // project
        case 63:   // projectFields
        case 64:   // projection
        case 65:   // compoundExpression
        case 66:   // expressionArray
        case 67:   // expressionObject
        case 68:   // expressionFields
        case 69:   // exprFixedTwoArg
        case 70:   // expression
        case 71:   // maths
        case 72:   // add
        case 73:   // atan2
        case 74:   // string
        case 75:   // binary
        case 76:   // undefined
        case 77:   // objectId
        case 78:   // bool
        case 79:   // date
        case 80:   // null
        case 81:   // regex
        case 82:   // dbPointer
        case 83:   // javascript
        case 84:   // symbol
        case 85:   // javascriptWScope
        case 86:   // int
        case 87:   // timestamp
        case 88:   // long
        case 89:   // double
        case 90:   // decimal
        case 91:   // minKey
        case 92:   // maxKey
        case 93:   // simpleValue
        case 94:   // boolExps
        case 95:   // and
        case 96:   // or
        case 97:   // not
        case 98:   // literalEscapes
        case 99:   // const
        case 100:  // literal
        case 101:  // value
        case 102:  // compoundValue
        case 103:  // valueArray
        case 104:  // valueObject
        case 105:  // valueFields
        case 106:  // compExprs
        case 107:  // cmp
        case 108:  // eq
        case 109:  // gt
        case 110:  // gte
        case 111:  // lt
        case 112:  // lte
        case 113:  // ne
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 114:  // projectionFieldname
        case 115:  // expressionFieldname
        case 116:  // stageAsUserFieldname
        case 117:  // argAsUserFieldname
        case 118:  // aggExprAsUserFieldname
        case 119:  // invariableUserFieldname
        case 120:  // idAsUserFieldname
        case 121:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 40:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 51:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 39:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 48:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 53:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 52:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 41:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 38:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 50:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 47:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 49:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 122:  // projectField
        case 123:  // expressionField
        case 124:  // valueField
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 35:  // FIELDNAME
        case 36:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 125:  // expressions
        case 126:  // values
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
    switch (that.type_get()) {
        case 37:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 44:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 46:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 43:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 42:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 45:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 55:   // stageList
        case 56:   // stage
        case 57:   // inhibitOptimization
        case 58:   // unionWith
        case 59:   // num
        case 60:   // skip
        case 61:   // limit
        case 62:   // project
        case 63:   // projectFields
        case 64:   // projection
        case 65:   // compoundExpression
        case 66:   // expressionArray
        case 67:   // expressionObject
        case 68:   // expressionFields
        case 69:   // exprFixedTwoArg
        case 70:   // expression
        case 71:   // maths
        case 72:   // add
        case 73:   // atan2
        case 74:   // string
        case 75:   // binary
        case 76:   // undefined
        case 77:   // objectId
        case 78:   // bool
        case 79:   // date
        case 80:   // null
        case 81:   // regex
        case 82:   // dbPointer
        case 83:   // javascript
        case 84:   // symbol
        case 85:   // javascriptWScope
        case 86:   // int
        case 87:   // timestamp
        case 88:   // long
        case 89:   // double
        case 90:   // decimal
        case 91:   // minKey
        case 92:   // maxKey
        case 93:   // simpleValue
        case 94:   // boolExps
        case 95:   // and
        case 96:   // or
        case 97:   // not
        case 98:   // literalEscapes
        case 99:   // const
        case 100:  // literal
        case 101:  // value
        case 102:  // compoundValue
        case 103:  // valueArray
        case 104:  // valueObject
        case 105:  // valueFields
        case 106:  // compExprs
        case 107:  // cmp
        case 108:  // eq
        case 109:  // gt
        case 110:  // gte
        case 111:  // lt
        case 112:  // lte
        case 113:  // ne
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 114:  // projectionFieldname
        case 115:  // expressionFieldname
        case 116:  // stageAsUserFieldname
        case 117:  // argAsUserFieldname
        case 118:  // aggExprAsUserFieldname
        case 119:  // invariableUserFieldname
        case 120:  // idAsUserFieldname
        case 121:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 40:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 51:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 39:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 48:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 53:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 52:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 41:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 38:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 50:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 47:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 49:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 122:  // projectField
        case 123:  // expressionField
        case 124:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 35:  // FIELDNAME
        case 36:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 125:  // expressions
        case 126:  // values
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.type = empty_symbol;
}

#if YY_CPLUSPLUS < 201103L
PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.type_get()) {
        case 37:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 44:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 46:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 43:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 42:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 45:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 55:   // stageList
        case 56:   // stage
        case 57:   // inhibitOptimization
        case 58:   // unionWith
        case 59:   // num
        case 60:   // skip
        case 61:   // limit
        case 62:   // project
        case 63:   // projectFields
        case 64:   // projection
        case 65:   // compoundExpression
        case 66:   // expressionArray
        case 67:   // expressionObject
        case 68:   // expressionFields
        case 69:   // exprFixedTwoArg
        case 70:   // expression
        case 71:   // maths
        case 72:   // add
        case 73:   // atan2
        case 74:   // string
        case 75:   // binary
        case 76:   // undefined
        case 77:   // objectId
        case 78:   // bool
        case 79:   // date
        case 80:   // null
        case 81:   // regex
        case 82:   // dbPointer
        case 83:   // javascript
        case 84:   // symbol
        case 85:   // javascriptWScope
        case 86:   // int
        case 87:   // timestamp
        case 88:   // long
        case 89:   // double
        case 90:   // decimal
        case 91:   // minKey
        case 92:   // maxKey
        case 93:   // simpleValue
        case 94:   // boolExps
        case 95:   // and
        case 96:   // or
        case 97:   // not
        case 98:   // literalEscapes
        case 99:   // const
        case 100:  // literal
        case 101:  // value
        case 102:  // compoundValue
        case 103:  // valueArray
        case 104:  // valueObject
        case 105:  // valueFields
        case 106:  // compExprs
        case 107:  // cmp
        case 108:  // eq
        case 109:  // gt
        case 110:  // gte
        case 111:  // lt
        case 112:  // lte
        case 113:  // ne
            value.copy<CNode>(that.value);
            break;

        case 114:  // projectionFieldname
        case 115:  // expressionFieldname
        case 116:  // stageAsUserFieldname
        case 117:  // argAsUserFieldname
        case 118:  // aggExprAsUserFieldname
        case 119:  // invariableUserFieldname
        case 120:  // idAsUserFieldname
        case 121:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 40:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 51:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 39:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 48:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 53:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 52:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 41:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 38:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 50:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 47:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 49:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 122:  // projectField
        case 123:  // expressionField
        case 124:  // valueField
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 35:  // FIELDNAME
        case 36:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 125:  // expressions
        case 126:  // values
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
    switch (that.type_get()) {
        case 37:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 44:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 46:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 43:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 42:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 45:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 55:   // stageList
        case 56:   // stage
        case 57:   // inhibitOptimization
        case 58:   // unionWith
        case 59:   // num
        case 60:   // skip
        case 61:   // limit
        case 62:   // project
        case 63:   // projectFields
        case 64:   // projection
        case 65:   // compoundExpression
        case 66:   // expressionArray
        case 67:   // expressionObject
        case 68:   // expressionFields
        case 69:   // exprFixedTwoArg
        case 70:   // expression
        case 71:   // maths
        case 72:   // add
        case 73:   // atan2
        case 74:   // string
        case 75:   // binary
        case 76:   // undefined
        case 77:   // objectId
        case 78:   // bool
        case 79:   // date
        case 80:   // null
        case 81:   // regex
        case 82:   // dbPointer
        case 83:   // javascript
        case 84:   // symbol
        case 85:   // javascriptWScope
        case 86:   // int
        case 87:   // timestamp
        case 88:   // long
        case 89:   // double
        case 90:   // decimal
        case 91:   // minKey
        case 92:   // maxKey
        case 93:   // simpleValue
        case 94:   // boolExps
        case 95:   // and
        case 96:   // or
        case 97:   // not
        case 98:   // literalEscapes
        case 99:   // const
        case 100:  // literal
        case 101:  // value
        case 102:  // compoundValue
        case 103:  // valueArray
        case 104:  // valueObject
        case 105:  // valueFields
        case 106:  // compExprs
        case 107:  // cmp
        case 108:  // eq
        case 109:  // gt
        case 110:  // gte
        case 111:  // lt
        case 112:  // lte
        case 113:  // ne
            value.move<CNode>(that.value);
            break;

        case 114:  // projectionFieldname
        case 115:  // expressionFieldname
        case 116:  // stageAsUserFieldname
        case 117:  // argAsUserFieldname
        case 118:  // aggExprAsUserFieldname
        case 119:  // invariableUserFieldname
        case 120:  // idAsUserFieldname
        case 121:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 40:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 51:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 39:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 48:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 53:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 52:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 41:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 38:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 50:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 47:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 49:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 122:  // projectField
        case 123:  // expressionField
        case 124:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 35:  // FIELDNAME
        case 36:  // STRING
            value.move<std::string>(that.value);
            break;

        case 125:  // expressions
        case 126:  // values
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
    symbol_number_type yytype = yysym.type_get();
#if defined __GNUC__ && !defined __clang__ && !defined __ICC && \
    __GNUC__ * 100 + __GNUC_MINOR__ <= 408
    // Avoid a (spurious) G++ 4.8 warning about "array subscript is
    // below array bounds".
    if (yysym.empty())
        std::abort();
#endif
    yyo << (yytype < yyntokens_ ? "token" : "nterm") << ' ' << yytname_[yytype] << " ("
        << yysym.location << ": ";
    YYUSE(yytype);
    yyo << ')';
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
    int yyr = yypgoto_[yysym - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - yyntokens_];
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
            YYCDEBUG << "Reading a token: ";
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

        /* If the proper action on seeing token YYLA.TYPE is to reduce or
           to detect an error, take that action.  */
        yyn += yyla.type_get();
        if (yyn < 0 || yylast_ < yyn || yycheck_[yyn] != yyla.type_get()) {
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
                case 37:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 44:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 46:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 43:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 42:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 45:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 55:   // stageList
                case 56:   // stage
                case 57:   // inhibitOptimization
                case 58:   // unionWith
                case 59:   // num
                case 60:   // skip
                case 61:   // limit
                case 62:   // project
                case 63:   // projectFields
                case 64:   // projection
                case 65:   // compoundExpression
                case 66:   // expressionArray
                case 67:   // expressionObject
                case 68:   // expressionFields
                case 69:   // exprFixedTwoArg
                case 70:   // expression
                case 71:   // maths
                case 72:   // add
                case 73:   // atan2
                case 74:   // string
                case 75:   // binary
                case 76:   // undefined
                case 77:   // objectId
                case 78:   // bool
                case 79:   // date
                case 80:   // null
                case 81:   // regex
                case 82:   // dbPointer
                case 83:   // javascript
                case 84:   // symbol
                case 85:   // javascriptWScope
                case 86:   // int
                case 87:   // timestamp
                case 88:   // long
                case 89:   // double
                case 90:   // decimal
                case 91:   // minKey
                case 92:   // maxKey
                case 93:   // simpleValue
                case 94:   // boolExps
                case 95:   // and
                case 96:   // or
                case 97:   // not
                case 98:   // literalEscapes
                case 99:   // const
                case 100:  // literal
                case 101:  // value
                case 102:  // compoundValue
                case 103:  // valueArray
                case 104:  // valueObject
                case 105:  // valueFields
                case 106:  // compExprs
                case 107:  // cmp
                case 108:  // eq
                case 109:  // gt
                case 110:  // gte
                case 111:  // lt
                case 112:  // lte
                case 113:  // ne
                    yylhs.value.emplace<CNode>();
                    break;

                case 114:  // projectionFieldname
                case 115:  // expressionFieldname
                case 116:  // stageAsUserFieldname
                case 117:  // argAsUserFieldname
                case 118:  // aggExprAsUserFieldname
                case 119:  // invariableUserFieldname
                case 120:  // idAsUserFieldname
                case 121:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 40:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 51:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 39:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 48:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 53:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 52:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 41:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 38:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 50:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 47:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 49:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 122:  // projectField
                case 123:  // expressionField
                case 124:  // valueField
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 35:  // FIELDNAME
                case 36:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 125:  // expressions
                case 126:  // values
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
#line 198 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1328 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 204 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 205 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1342 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 213 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1348 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 216 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1354 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 216 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1360 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 216 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1366 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 216 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1372 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 216 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1378 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 220 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1386 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 226 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1399 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 236 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1405 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 236 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1411 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 236 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1417 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 236 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1423 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 240 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1431 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 245 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1439 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 250 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1447 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 256 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1455 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 259 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1464 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 266 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1472 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 269 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1480 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 275 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1486 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 276 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1494 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 279 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1502 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 282 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1510 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 285 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1518 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 288 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1526 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 291 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1534 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 294 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1542 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 297 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1550 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 300 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1558 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 303 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1566 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 306 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1572 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1578 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1584 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1590 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1596 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 314 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1604 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 322 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1612 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1620 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 328 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1628 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 331 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1636 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 334 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1644 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 343 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1652 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 346 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1660 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 354 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1668 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 357 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1676 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 360 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 363 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 1692 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 366 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 1700 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 369 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1708 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 372 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 375 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 1724 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 378 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 1732 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 1740 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 384 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 1748 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 387 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 1756 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 1764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 393 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 1772 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 399 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 1780 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 405 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 1788 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 411 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 1796 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 417 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 1804 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 423 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 1812 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 1820 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 435 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 1828 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 441 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 1836 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 447 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 1844 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 453 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 1852 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 459 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 1860 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 465 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 1868 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 471 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 1876 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 477 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 1884 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 483 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1892 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 486 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0}};
                    }
#line 1900 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 492 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1908 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 495 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 1916 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 501 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1924 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 504 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 1932 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 510 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1940 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 513 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 1948 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 519 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 1956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 522 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 1964 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 528 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1970 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 529 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1976 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 530 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1982 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 531 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1988 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 532 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1994 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 533 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2000 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 534 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2006 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 535 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2012 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 536 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2018 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 537 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2024 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 538 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2030 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 539 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2036 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 540 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2042 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 541 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2048 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 542 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2054 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 543 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2060 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 544 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2066 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 545 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2072 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 546 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2078 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 553 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2084 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 554 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2093 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 561 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2099 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 561 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2105 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 565 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2113 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2119 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2125 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2131 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2137 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2143 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2149 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 576 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2157 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 584 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2165 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 590 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2173 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 593 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2182 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 600 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2190 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2196 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2202 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2208 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2214 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 611 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2222 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 617 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2228 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 618 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2234 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 622 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2246 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 632 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2255 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 639 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2261 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 639 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2267 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 639 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2273 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 643 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2285 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 653 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2297 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 663 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2306 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 670 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2312 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 670 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2318 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 674 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2327 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 681 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2336 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 688 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2342 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 688 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2348 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 692 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2354 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 692 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2360 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 696 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2368 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 702 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2374 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 703 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 710 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2391 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 716 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2399 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 719 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2408 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 726 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2416 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 733 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 734 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 735 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2434 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 736 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2440 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 737 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2446 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2452 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2458 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2464 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2470 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2476 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2482 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 740 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2488 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 742 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2497 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 747 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2506 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 752 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2515 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 757 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2524 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 762 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2533 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 767 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2542 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 772 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2551 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 2555 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
            YY_STACK_PRINT();

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
            error(yyla.location, yysyntax_error_(yystack_[0].state, yyla));
        }


        yyerror_range[1].location = yyla.location;
        if (yyerrstatus_ == 3) {
            /* If just tried and failed to reuse lookahead token after an
               error, discard it.  */

            // Return failure if at end of input.
            if (yyla.type_get() == yyeof_)
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
        goto yyerrlab1;


    /*-------------------------------------------------------------.
    | yyerrlab1 -- common code for both syntax error and YYERROR.  |
    `-------------------------------------------------------------*/
    yyerrlab1:
        yyerrstatus_ = 3;  // Each real token shifted decrements this.
        {
            stack_symbol_type error_token;
            for (;;) {
                yyn = yypact_[+yystack_[0].state];
                if (!yy_pact_value_is_default_(yyn)) {
                    yyn += yy_error_token_;
                    if (0 <= yyn && yyn <= yylast_ && yycheck_[yyn] == yy_error_token_) {
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

// Generate an error message.
std::string PipelineParserGen::yysyntax_error_(state_type, const symbol_type&) const {
    return YY_("syntax error");
}


const short PipelineParserGen::yypact_ninf_ = -165;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    29,   40,   47,   13,   45,   -165, 49,   -165, 14,   14,   51,   56,   -165, -165, -165, -165,
    -165, -165, 58,   48,   63,   -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165,
    -165, -165, -165, -165, 40,   -165, 33,   -165, 171,  -165, -165, 52,   -165, 8,    -165, -165,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165,
    -165, -165, -165, -165, 8,    -165, -165, -165, -165, -165, -2,   250,  74,   -165, -165, -165,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, 70,
    71,   73,   75,   83,   84,   88,   89,   73,   73,   73,   73,   73,   73,   73,   235,  -165,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, 74,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165,
    -165, -165, -165, -165, 90,   -165, 74,   74,   94,   74,   92,   92,   74,   74,   95,   102,
    103,  104,  105,  144,  146,  -165, -165, 74,   -165, -165, -165, -165, -165, -165, -165, 74,
    74,   -165, 74,   -165, 92,   -165, 145,  -165, -165, -165, 148,  74,   149,  -165, -165, -165,
    -165, -165, -165, -165, -165, 74,   150,  74,   203,  92,   151,  154,  155,  74,   156,  158,
    -165, 159,  -165, -165, -165, -165, -165, -165, 92,   -165, -165, -165, -165, -165, 160,  -165,
    157,  163,  -165, 164,  -165, -165, -165};

const unsigned char PipelineParserGen::yydefact_[] = {
    0,   3,   0,   0,   0,   1,   0,   5,   0,   0,   0,   0,   7,   8,   9,   10,  11,  2,   0,
    0,   0,   78,  80,  82,  84,  77,  79,  81,  83,  18,  14,  15,  16,  17,  19,  21,  3,   12,
    0,   6,   0,   4,   63,  0,   20,  0,   42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  41,  0,   38,  39,  40,  37,  22,  0,   119,
    106, 27,  29,  31,  33,  34,  35,  26,  28,  30,  32,  23,  36,  111, 112, 113, 127, 128, 25,
    114, 131, 132, 133, 115, 137, 138, 116, 157, 158, 159, 160, 161, 162, 163, 24,  0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   85,  86,  64,  65,  66,  67,
    68,  69,  70,  71,  72,  73,  74,  75,  76,  109, 106, 87,  88,  89,  90,  102, 91,  92,  93,
    94,  95,  96,  97,  98,  103, 99,  100, 101, 104, 105, 108, 0,   13,  0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   118, 126, 0,   123, 124, 122, 125, 120, 107,
    117, 0,   0,   130, 0,   149, 146, 141, 0,   142, 143, 144, 0,   0,   0,   164, 165, 166, 167,
    168, 169, 170, 121, 106, 0,   106, 0,   146, 0,   0,   0,   106, 0,   0,   110, 0,   148, 153,
    154, 155, 152, 156, 0,   150, 147, 145, 139, 140, 0,   136, 0,   0,   151, 0,   129, 134, 135};

const short PipelineParserGen::yypgoto_[] = {
    -165, 126,  -165, -165, -165, 161,  -165, -165, -165, -165, 101,  -33,  -165, -165, -165, -83,
    -20,  -165, -165, -165, -12,  -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165,
    6,    -165, 37,   -4,   41,   -165, -165, -164, -165, -165, -165, -165, -165, -165, -165, -161,
    -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -165, -126, -125,
    -43,  -124, -40,  -165, -165, -165, -165, -140, -38,  -165, -165, -165};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  4,   11,  12,  13,  29,  14,  15,  16,  40,  87,  142, 89,  90,  126, 168, 143, 91,  92,
    93,  144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161,
    162, 163, 95,  96,  97,  98,  99,  100, 101, 217, 199, 200, 201, 216, 102, 103, 104, 105, 106,
    107, 108, 109, 68,  183, 69,  70,  71,  72,  187, 232, 73,  188, 233, 164, 218, 2,   19,  20};

const unsigned char PipelineParserGen::yytable_[] = {
    184, 185, 186, 189, 32,  32,  197, 197, 23,  198, 202, 75,  88,  76,  30,  30,  77,  78,  79,
    80,  81,  82,  21,  22,  23,  24,  43,  6,   7,   8,   9,   10,  197, 94,  1,   88,  174, 175,
    176, 177, 178, 179, 180, 3,   42,  31,  31,  5,   27,  33,  33,  17,  18,  197, 35,  83,  94,
    84,  85,  86,  36,  25,  37,  26,  27,  28,  39,  38,  197, 42,  111, 242, 74,  223, 165, 225,
    166, 75,  167, 76,  169, 238, 21,  22,  23,  24,  127, 128, 170, 171, 227, 228, 230, 172, 173,
    195, 190, 196, 193, 205, 21,  22,  23,  24,  127, 128, 206, 207, 208, 209, 42,  129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 25,  139, 26,  27,  28,  140, 141, 42,  129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 25,  139, 26,  27,  28,  140, 141, 191, 192, 210, 194, 211, 219,
    203, 204, 220, 222, 224, 235, 236, 237, 239, 244, 41,  212, 240, 241, 243, 245, 246, 110, 34,
    213, 214, 229, 215, 44,  231, 0,   45,  234, 0,   0,   0,   221, 0,   46,  47,  48,  49,  50,
    51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  226, 0,
    0,   182, 0,   0,   0,   0,   0,   0,   46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  181, 0,   0,   182, 0,   0,   0,   0,
    0,   0,   46,  47,  48,  49,  50,  51,  52,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   67,  112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125};

const short PipelineParserGen::yycheck_[] = {
    126, 126, 126, 143, 8,   9,   170, 171, 10,  170, 171, 3,   45,  5,   8,   9,   8,   9,   10,
    11,  12,  13,  8,   9,   10,  11,  38,  14,  15,  16,  17,  18,  196, 45,  5,   68,  119, 120,
    121, 122, 123, 124, 125, 3,   36,  8,   9,   0,   50,  8,   9,   6,   3,   217, 3,   47,  68,
    49,  50,  51,  4,   47,  4,   49,  50,  51,  3,   19,  232, 36,  74,  232, 20,  213, 4,   215,
    5,   3,   5,   5,   5,   221, 8,   9,   10,  11,  12,  13,  5,   5,   216, 216, 216, 5,   5,
    3,   6,   5,   4,   4,   8,   9,   10,  11,  12,  13,  4,   4,   4,   4,   36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  166, 167, 4,   169, 4,   6,
    172, 173, 6,   6,   6,   6,   4,   4,   4,   4,   36,  183, 6,   6,   6,   4,   4,   68,  9,
    191, 192, 216, 194, 4,   216, -1,  7,   217, -1,  -1,  -1,  203, -1,  14,  15,  16,  17,  18,
    19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  4,   -1,
    -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,
    -1,  -1,  14,  15,  16,  17,  18,  19,  20,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  35,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   5,   127, 3,   55,  0,   14,  15,  16,  17,  18,  56,  57,  58,  60,  61,  62,  6,   3,
    128, 129, 8,   9,   10,  11,  47,  49,  50,  51,  59,  86,  88,  89,  90,  59,  3,   4,   4,
    19,  3,   63,  55,  36,  74,  4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  114, 116, 117, 118, 119, 122, 20,  3,
    5,   8,   9,   10,  11,  12,  13,  47,  49,  50,  51,  64,  65,  66,  67,  71,  72,  73,  74,
    94,  95,  96,  97,  98,  99,  100, 106, 107, 108, 109, 110, 111, 112, 113, 64,  89,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  68,  12,  13,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  48,  52,  53,  65,  70,  74,  75,  76,  77,  78,  79,  80,  81,
    82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  125, 4,   5,   5,   69,  5,   5,
    5,   5,   5,   69,  69,  69,  69,  69,  69,  69,  4,   7,   115, 116, 117, 119, 120, 123, 125,
    6,   70,  70,  4,   70,  3,   5,   93,  101, 102, 103, 104, 101, 70,  70,  4,   4,   4,   4,
    4,   4,   4,   70,  70,  70,  70,  105, 101, 126, 6,   6,   70,  6,   125, 6,   125, 4,   116,
    117, 118, 119, 120, 121, 124, 126, 6,   4,   4,   125, 4,   6,   6,   101, 6,   4,   4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   54,  127, 55,  55,  129, 128, 56,  56,  56,  56,  56,  57,  58,  59,  59,  59,  59,  60,
    61,  62,  63,  63,  122, 122, 64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  114,
    114, 114, 114, 119, 116, 116, 116, 116, 116, 117, 117, 118, 118, 118, 118, 118, 118, 118, 118,
    118, 118, 118, 118, 118, 118, 74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  85,  87,  91,
    92,  86,  86,  88,  88,  89,  89,  90,  90,  78,  78,  93,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  125, 125, 70,  70,  69,  65,  65,  65,
    65,  65,  65,  66,  67,  68,  68,  123, 115, 115, 115, 115, 120, 71,  71,  72,  73,  94,  94,
    94,  95,  96,  97,  98,  98,  99,  100, 101, 101, 102, 102, 103, 126, 126, 104, 105, 105, 124,
    121, 121, 121, 121, 121, 106, 106, 106, 106, 106, 106, 106, 107, 108, 109, 110, 111, 112, 113};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 4, 1, 1, 1, 1, 1,
    1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 8, 4, 1, 1, 1, 8, 8, 6, 1, 1, 6, 6, 1, 1, 1, 1,
    3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4};


#if YYDEBUG
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a yyntokens_, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "$undefined",
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
                                                   "CONST_EXPR",
                                                   "LITERAL",
                                                   "OR",
                                                   "NOT",
                                                   "CMP",
                                                   "EQ",
                                                   "GT",
                                                   "GTE",
                                                   "LT",
                                                   "LTE",
                                                   "NE",
                                                   "FIELDNAME",
                                                   "STRING",
                                                   "BINARY",
                                                   "UNDEFINED",
                                                   "OBJECT_ID",
                                                   "DATE_LITERAL",
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
                                                   "exprFixedTwoArg",
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
                                                   "compExprs",
                                                   "cmp",
                                                   "eq",
                                                   "gt",
                                                   "gte",
                                                   "lt",
                                                   "lte",
                                                   "ne",
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


const short PipelineParserGen::yyrline_[] = {
    0,   198, 198, 204, 205, 213, 213, 216, 216, 216, 216, 216, 220, 226, 236, 236, 236, 236, 240,
    245, 250, 256, 259, 266, 269, 275, 276, 279, 282, 285, 288, 291, 294, 297, 300, 303, 306, 310,
    310, 310, 310, 314, 322, 325, 328, 331, 334, 343, 346, 354, 357, 360, 363, 366, 369, 372, 375,
    378, 381, 384, 387, 390, 393, 399, 405, 411, 417, 423, 429, 435, 441, 447, 453, 459, 465, 471,
    477, 483, 486, 492, 495, 501, 504, 510, 513, 519, 522, 528, 529, 530, 531, 532, 533, 534, 535,
    536, 537, 538, 539, 540, 541, 542, 543, 544, 545, 546, 553, 554, 561, 561, 565, 570, 570, 570,
    570, 570, 570, 576, 584, 590, 593, 600, 607, 607, 607, 607, 611, 617, 618, 622, 632, 639, 639,
    639, 643, 653, 663, 670, 670, 674, 681, 688, 688, 692, 692, 696, 702, 703, 710, 716, 719, 726,
    733, 734, 735, 736, 737, 740, 740, 740, 740, 740, 740, 740, 742, 747, 752, 757, 762, 767, 772};

// Print the state stack on the debug stream.
void PipelineParserGen::yystack_print_() {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

// Report on the debug stream that the rule \a yyrule is going to be reduced.
void PipelineParserGen::yy_reduce_print_(int yyrule) {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 3060 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 777 "src/mongo/db/cst/pipeline_grammar.yy"
