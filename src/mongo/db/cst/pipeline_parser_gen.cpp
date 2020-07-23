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
        case 39:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 46:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 45:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 44:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 47:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 42:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 41:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 50:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 55:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 54:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 43:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 40:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 49:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 51:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 128:  // expressions
        case 129:  // values
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
        case 39:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 46:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 45:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 44:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 47:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 42:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 41:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 50:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 55:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 54:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 43:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 40:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 49:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 51:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 128:  // expressions
        case 129:  // values
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
        case 39:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 46:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 45:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 44:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 47:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.copy<CNode>(that.value);
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 42:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 41:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 50:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 55:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 54:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 43:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 40:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 49:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 51:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 128:  // expressions
        case 129:  // values
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
        case 39:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 46:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 48:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 45:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 44:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 47:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 68:   // dbPointer
        case 69:   // javascript
        case 70:   // symbol
        case 71:   // javascriptWScope
        case 72:   // int
        case 73:   // timestamp
        case 74:   // long
        case 75:   // double
        case 76:   // decimal
        case 77:   // minKey
        case 78:   // maxKey
        case 79:   // value
        case 80:   // string
        case 81:   // binary
        case 82:   // undefined
        case 83:   // objectId
        case 84:   // bool
        case 85:   // date
        case 86:   // null
        case 87:   // regex
        case 88:   // simpleValue
        case 89:   // compoundValue
        case 90:   // valueArray
        case 91:   // valueObject
        case 92:   // valueFields
        case 93:   // stageList
        case 94:   // stage
        case 95:   // inhibitOptimization
        case 96:   // unionWith
        case 97:   // skip
        case 98:   // limit
        case 99:   // project
        case 100:  // sample
        case 101:  // projectFields
        case 102:  // projection
        case 103:  // num
        case 104:  // expression
        case 105:  // compoundExpression
        case 106:  // exprFixedTwoArg
        case 107:  // expressionArray
        case 108:  // expressionObject
        case 109:  // expressionFields
        case 110:  // maths
        case 111:  // add
        case 112:  // atan2
        case 113:  // boolExps
        case 114:  // and
        case 115:  // or
        case 116:  // not
        case 117:  // literalEscapes
        case 118:  // const
        case 119:  // literal
        case 120:  // compExprs
        case 121:  // cmp
        case 122:  // eq
        case 123:  // gt
        case 124:  // gte
        case 125:  // lt
        case 126:  // lte
        case 127:  // ne
            value.move<CNode>(that.value);
            break;

        case 57:  // projectionFieldname
        case 58:  // expressionFieldname
        case 59:  // stageAsUserFieldname
        case 60:  // argAsUserFieldname
        case 61:  // aggExprAsUserFieldname
        case 62:  // invariableUserFieldname
        case 63:  // idAsUserFieldname
        case 64:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 42:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 53:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 41:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 50:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 55:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 54:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 43:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 40:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 52:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 49:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 51:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 65:  // projectField
        case 66:  // expressionField
        case 67:  // valueField
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 37:  // FIELDNAME
        case 38:  // STRING
            value.move<std::string>(that.value);
            break;

        case 128:  // expressions
        case 129:  // values
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
                case 39:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 46:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 48:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 45:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 44:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 47:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 68:   // dbPointer
                case 69:   // javascript
                case 70:   // symbol
                case 71:   // javascriptWScope
                case 72:   // int
                case 73:   // timestamp
                case 74:   // long
                case 75:   // double
                case 76:   // decimal
                case 77:   // minKey
                case 78:   // maxKey
                case 79:   // value
                case 80:   // string
                case 81:   // binary
                case 82:   // undefined
                case 83:   // objectId
                case 84:   // bool
                case 85:   // date
                case 86:   // null
                case 87:   // regex
                case 88:   // simpleValue
                case 89:   // compoundValue
                case 90:   // valueArray
                case 91:   // valueObject
                case 92:   // valueFields
                case 93:   // stageList
                case 94:   // stage
                case 95:   // inhibitOptimization
                case 96:   // unionWith
                case 97:   // skip
                case 98:   // limit
                case 99:   // project
                case 100:  // sample
                case 101:  // projectFields
                case 102:  // projection
                case 103:  // num
                case 104:  // expression
                case 105:  // compoundExpression
                case 106:  // exprFixedTwoArg
                case 107:  // expressionArray
                case 108:  // expressionObject
                case 109:  // expressionFields
                case 110:  // maths
                case 111:  // add
                case 112:  // atan2
                case 113:  // boolExps
                case 114:  // and
                case 115:  // or
                case 116:  // not
                case 117:  // literalEscapes
                case 118:  // const
                case 119:  // literal
                case 120:  // compExprs
                case 121:  // cmp
                case 122:  // eq
                case 123:  // gt
                case 124:  // gte
                case 125:  // lt
                case 126:  // lte
                case 127:  // ne
                    yylhs.value.emplace<CNode>();
                    break;

                case 57:  // projectionFieldname
                case 58:  // expressionFieldname
                case 59:  // stageAsUserFieldname
                case 60:  // argAsUserFieldname
                case 61:  // aggExprAsUserFieldname
                case 62:  // invariableUserFieldname
                case 63:  // idAsUserFieldname
                case 64:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 42:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 53:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 41:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 50:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 55:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 54:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 43:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 40:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 52:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 49:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 51:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 65:  // projectField
                case 66:  // expressionField
                case 67:  // valueField
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 37:  // FIELDNAME
                case 38:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 128:  // expressions
                case 129:  // values
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
#line 211 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1333 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 217 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1339 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 218 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1347 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 226 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1353 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1359 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1365 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1371 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1377 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 229 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1389 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 232 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1401 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 242 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1409 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 258 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 258 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1434 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 258 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1440 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 258 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1446 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 262 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1454 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 267 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1462 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 272 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1470 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 278 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1478 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 281 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1487 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 288 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1495 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 291 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1503 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 297 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1509 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 298 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1517 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 301 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1525 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 304 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1533 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 307 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1541 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1549 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 313 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1557 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 316 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1565 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 319 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1573 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 322 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1581 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1589 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 328 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1595 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 332 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1601 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 332 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1607 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 332 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1613 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 332 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1619 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 336 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1627 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 344 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1635 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 347 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1643 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 350 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1651 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 353 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1659 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 356 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1667 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 359 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 1675 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 368 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1683 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 371 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1691 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 374 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 1699 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 382 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1707 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 385 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1715 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 388 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1723 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 1731 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 394 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 1739 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 397 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1747 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 400 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1755 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 403 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 1763 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 406 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 1771 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 409 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 1779 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 412 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 1787 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 415 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 1795 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 418 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 1803 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 421 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 1811 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 428 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 1819 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 434 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 1827 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 440 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 1835 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 446 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 1843 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 452 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 1851 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 1859 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 464 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 1867 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 470 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 1875 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 476 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 1883 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 482 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 1891 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 488 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 1899 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 494 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 1907 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 500 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 1915 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 506 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 1923 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 512 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1931 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 515 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 1939 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 521 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1947 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 524 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 1955 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 530 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1963 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 533 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 1971 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 539 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1979 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 542 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 1987 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 548 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 1995 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 551 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2003 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 557 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2009 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 558 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2015 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 559 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2021 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 560 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2027 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 561 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2033 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 562 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2039 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 563 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2045 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 564 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2051 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 565 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2057 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 566 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2063 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 567 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2069 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 568 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2075 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 569 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2081 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 570 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2087 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 571 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2093 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 572 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2099 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 573 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2105 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 574 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2111 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 575 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2117 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 582 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2123 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 583 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2132 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 590 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2138 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 590 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2144 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 594 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2152 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2158 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2164 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2170 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2176 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2182 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 599 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2188 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 605 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2196 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 613 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2204 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 619 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2212 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 622 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2221 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 629 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2229 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 636 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2235 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 636 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2241 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 636 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2247 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 636 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2253 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 640 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2261 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 646 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2267 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 647 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2273 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 651 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2285 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 661 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2294 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 668 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2300 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 668 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2306 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 668 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2312 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 672 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2324 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 682 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2336 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 692 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2345 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 699 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2351 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 699 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2357 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 703 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2366 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 710 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2375 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 717 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2381 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 717 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2387 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 721 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2393 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 721 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2399 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 725 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2407 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 731 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2413 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 732 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 739 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2430 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 745 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2438 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 748 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2447 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 755 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2455 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 762 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2461 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 763 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2467 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 764 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2473 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 765 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2479 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 766 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2485 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2491 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2497 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2503 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2509 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2515 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2521 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2527 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 771 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2536 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 776 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2545 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 781 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2554 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 786 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2563 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 791 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2572 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 796 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2581 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 801 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2590 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 2594 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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


const short PipelineParserGen::yypact_ninf_ = -173;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    14,   17,   10,   151,  21,   -173, 25,   3,    27,   28,   3,    -173, 35,   -173, -173, -173,
    -173, -173, -173, -173, 36,   -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, 15,   -173, 40,   44,   17,   -173, 173,  3,    12,   -173, -173, -173, 13,
    -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, -173, -173, 13,   -173, -173, -173, -173, -173, 63,   -173,
    68,   256,  33,   -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, 5,    43,   85,   86,   92,   93,   100,  101,  85,   85,
    85,   85,   85,   85,   85,   241,  -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, -173, -173, -173, 33,   -173, 102,  103,  33,   33,   105,
    33,   91,   91,   33,   33,   106,  108,  111,  114,  117,  118,  120,  -173, -173, 33,   -173,
    -173, -173, -173, -173, -173, -173, -173, 33,   33,   -173, 33,   -173, 91,   119,  -173, -173,
    -173, -173, 121,  33,   122,  -173, -173, -173, -173, -173, -173, -173, -173, 33,   146,  33,
    207,  91,   157,  167,  168,  33,   171,  172,  -173, 175,  -173, -173, -173, -173, -173, -173,
    91,   -173, -173, -173, -173, -173, 176,  -173, 179,  180,  -173, 182,  -173, -173, -173};

const unsigned char PipelineParserGen::yydefact_[] = {
    0,   3,   0,   0,   0,   1,   0,   0,   0,   0,   0,   5,   0,   7,   8,   9,   10,  11,  12,
    2,   0,   82,  84,  86,  88,  81,  83,  85,  87,  16,  17,  18,  19,  21,  23,  0,   20,  0,
    0,   3,   14,  0,   0,   0,   6,   4,   22,  0,   44,  47,  48,  49,  46,  45,  50,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  43,  0,   40,  41,  42,
    39,  24,  0,   67,  0,   123, 110, 29,  31,  33,  35,  36,  37,  28,  30,  32,  34,  27,  25,
    38,  115, 116, 117, 131, 132, 118, 135, 136, 137, 119, 141, 142, 120, 161, 162, 163, 164, 165,
    166, 167, 26,  13,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   89,  90,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  98,  99,  100,
    101, 102, 107, 103, 104, 105, 108, 109, 91,  92,  93,  94,  106, 95,  96,  97,  112, 110, 113,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   122, 130,
    0,   127, 128, 126, 129, 124, 111, 121, 15,  0,   0,   134, 0,   153, 150, 0,   145, 146, 147,
    148, 0,   0,   0,   168, 169, 170, 171, 172, 173, 174, 125, 110, 0,   110, 0,   150, 0,   0,
    0,   110, 0,   0,   114, 0,   152, 157, 158, 159, 156, 160, 0,   154, 151, 149, 143, 144, 0,
    140, 0,   0,   155, 0,   133, 138, 139};

const short PipelineParserGen::yypgoto_[] = {
    -173, -173, -173, -132, -131, -111, -129, -98,  -173, -173, -173, -173, -173, -173, -173, -173,
    107,  -173, 109,  -7,   113,  -173, -173, -170, -14,  -173, -173, -173, -173, -173, -173, -173,
    -172, -173, -173, -173, -173, 140,  -173, -173, -173, -173, -173, -173, -173, -173, 141,  7,
    -26,  -13,  30,   -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173, -173,
    -173, -173, -173, -173, -173, -173, -173, -173, -160, -10,  -173, -173, -173};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  72,  190, 73,  74,  75,  76,  194, 240, 77,  195, 241, 149, 150, 151, 152,
    153, 154, 155, 156, 157, 158, 159, 225, 160, 161, 162, 163, 164, 165, 166, 167,
    168, 207, 208, 209, 224, 4,   12,  13,  14,  15,  16,  17,  18,  41,  94,  33,
    169, 170, 175, 96,  97,  133, 98,  99,  100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115, 171, 226, 2,   37,  38};

const unsigned char PipelineParserGen::yytable_[] = {
    31,  191, 192, 31,  193, 206, 206, 205, 210, 196, 5,   21,  22,  23,  24,  23,  81,  36,  82,
    1,   3,   83,  84,  85,  86,  87,  88,  19,  20,  80,  34,  35,  206, 93,  95,  31,  81,  42,
    82,  39,  40,  21,  22,  23,  24,  134, 135, 44,  173, 78,  79,  79,  25,  206, 26,  27,  28,
    27,  93,  95,  43,  231, 89,  233, 90,  91,  92,  117, 206, 246, 250, 79,  136, 137, 138, 139,
    140, 141, 142, 143, 144, 145, 25,  146, 26,  27,  28,  147, 148, 118, 174, 176, 235, 236, 203,
    238, 204, 177, 178, 21,  22,  23,  24,  134, 135, 179, 180, 198, 197, 201, 213, 172, 214, 237,
    29,  215, 30,  29,  216, 30,  32,  217, 218, 32,  219, 227, 239, 228, 230, 79,  136, 137, 138,
    139, 140, 141, 142, 143, 144, 145, 25,  146, 26,  27,  28,  147, 148, 199, 200, 29,  202, 30,
    232, 211, 212, 32,  181, 182, 183, 184, 185, 186, 187, 243, 220, 6,   7,   8,   9,   10,  11,
    244, 245, 221, 222, 247, 223, 46,  248, 45,  47,  249, 251, 252, 253, 229, 254, 48,  49,  50,
    51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
    70,  71,  234, 0,   116, 189, 242, 0,   0,   0,   0,   0,   48,  49,  50,  51,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  188, 0,
    0,   189, 0,   0,   0,   0,   0,   0,   48,  49,  50,  51,  52,  53,  54,  55,  56,  0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   71,  119, 120, 121, 122, 123, 124,
    125, 126, 127, 128, 129, 130, 131, 132};

const short PipelineParserGen::yycheck_[] = {
    7,   133, 133, 10,  133, 177, 178, 177, 178, 169, 0,   8,   9,   10,  11,  10,  3,   10,  5,
    5,   3,   8,   9,   10,  11,  12,  13,  6,   3,   43,  3,   3,   204, 47,  47,  42,  3,   22,
    5,   4,   4,   8,   9,   10,  11,  12,  13,  3,   5,   42,  38,  38,  49,  225, 51,  52,  53,
    52,  72,  72,  20,  221, 49,  223, 51,  52,  53,  4,   240, 229, 240, 38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  21,  5,   5,   224, 224, 3,
    224, 5,   5,   5,   8,   9,   10,  11,  12,  13,  5,   5,   4,   6,   4,   4,   118, 4,   224,
    7,   4,   7,   10,  4,   10,  7,   4,   4,   10,  4,   6,   224, 6,   6,   38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  173, 174, 42,  176, 42,
    6,   179, 180, 42,  126, 127, 128, 129, 130, 131, 132, 6,   190, 14,  15,  16,  17,  18,  19,
    4,   4,   199, 200, 4,   202, 4,   6,   39,  7,   6,   6,   4,   4,   211, 4,   14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,
    36,  37,  4,   -1,  72,  7,   225, -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  4,   -1,
    -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  37,  23,  24,  25,  26,  27,  28,
    29,  30,  31,  32,  33,  34,  35,  36};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   5,   130, 3,   93,  0,   14,  15,  16,  17,  18,  19,  94,  95,  96,  97,  98,  99,  100,
    6,   3,   8,   9,   10,  11,  49,  51,  52,  53,  72,  74,  75,  76,  103, 3,   3,   103, 131,
    132, 4,   4,   101, 22,  20,  3,   93,  4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  57,  59,  60,  61,
    62,  65,  103, 38,  80,  3,   5,   8,   9,   10,  11,  12,  13,  49,  51,  52,  53,  80,  102,
    105, 107, 108, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 102, 4,   21,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
    109, 12,  13,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  50,  54,  55,  68,  69,  70,
    71,  72,  73,  74,  75,  76,  77,  78,  80,  81,  82,  83,  84,  85,  86,  87,  88,  104, 105,
    128, 75,  5,   5,   106, 5,   5,   5,   5,   5,   106, 106, 106, 106, 106, 106, 106, 4,   7,
    58,  59,  60,  62,  63,  66,  128, 6,   4,   104, 104, 4,   104, 3,   5,   79,  88,  89,  90,
    91,  79,  104, 104, 4,   4,   4,   4,   4,   4,   4,   104, 104, 104, 104, 92,  79,  129, 6,
    6,   104, 6,   128, 6,   128, 4,   59,  60,  61,  62,  63,  64,  67,  129, 6,   4,   4,   128,
    4,   6,   6,   79,  6,   4,   4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   56,  130, 93,  93,  132, 131, 94,  94,  94,  94,  94,  94,  100, 95,  96,  103, 103,
    103, 103, 97,  98,  99,  101, 101, 65,  65,  102, 102, 102, 102, 102, 102, 102, 102, 102,
    102, 102, 102, 57,  57,  57,  57,  62,  59,  59,  59,  59,  59,  59,  60,  60,  60,  61,
    61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  61,  80,  81,  82,  83,  85,
    86,  87,  68,  69,  70,  71,  73,  77,  78,  72,  72,  74,  74,  75,  75,  76,  76,  84,
    84,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,  88,
    88,  88,  128, 128, 104, 104, 106, 105, 105, 105, 105, 105, 105, 107, 108, 109, 109, 66,
    58,  58,  58,  58,  63,  110, 110, 111, 112, 113, 113, 113, 114, 115, 116, 117, 117, 118,
    119, 79,  79,  89,  89,  90,  129, 129, 91,  92,  92,  67,  64,  64,  64,  64,  64,  120,
    120, 120, 120, 120, 120, 120, 121, 122, 123, 124, 125, 126, 127};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 4, 1, 1, 1, 1, 1,
    1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 8, 4, 1, 1, 1, 8, 8, 6, 1, 1, 6, 6, 1, 1, 1, 1, 3,
    0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "STAGE_SAMPLE",
                                                   "STAGE_SKIP",
                                                   "STAGE_UNION_WITH",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "SIZE_ARG",
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
                                                   "value",
                                                   "string",
                                                   "binary",
                                                   "undefined",
                                                   "objectId",
                                                   "bool",
                                                   "date",
                                                   "null",
                                                   "regex",
                                                   "simpleValue",
                                                   "compoundValue",
                                                   "valueArray",
                                                   "valueObject",
                                                   "valueFields",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "skip",
                                                   "limit",
                                                   "project",
                                                   "sample",
                                                   "projectFields",
                                                   "projection",
                                                   "num",
                                                   "expression",
                                                   "compoundExpression",
                                                   "exprFixedTwoArg",
                                                   "expressionArray",
                                                   "expressionObject",
                                                   "expressionFields",
                                                   "maths",
                                                   "add",
                                                   "atan2",
                                                   "boolExps",
                                                   "and",
                                                   "or",
                                                   "not",
                                                   "literalEscapes",
                                                   "const",
                                                   "literal",
                                                   "compExprs",
                                                   "cmp",
                                                   "eq",
                                                   "gt",
                                                   "gte",
                                                   "lt",
                                                   "lte",
                                                   "ne",
                                                   "expressions",
                                                   "values",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};


const short PipelineParserGen::yyrline_[] = {
    0,   211, 211, 217, 218, 226, 226, 229, 229, 229, 229, 229, 229, 232, 242, 248, 258, 258,
    258, 258, 262, 267, 272, 278, 281, 288, 291, 297, 298, 301, 304, 307, 310, 313, 316, 319,
    322, 325, 328, 332, 332, 332, 332, 336, 344, 347, 350, 353, 356, 359, 368, 371, 374, 382,
    385, 388, 391, 394, 397, 400, 403, 406, 409, 412, 415, 418, 421, 428, 434, 440, 446, 452,
    458, 464, 470, 476, 482, 488, 494, 500, 506, 512, 515, 521, 524, 530, 533, 539, 542, 548,
    551, 557, 558, 559, 560, 561, 562, 563, 564, 565, 566, 567, 568, 569, 570, 571, 572, 573,
    574, 575, 582, 583, 590, 590, 594, 599, 599, 599, 599, 599, 599, 605, 613, 619, 622, 629,
    636, 636, 636, 636, 640, 646, 647, 651, 661, 668, 668, 668, 672, 682, 692, 699, 699, 703,
    710, 717, 717, 721, 721, 725, 731, 732, 739, 745, 748, 755, 762, 763, 764, 765, 766, 769,
    769, 769, 769, 769, 769, 769, 771, 776, 781, 786, 791, 796, 801};

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
#line 3104 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 806 "src/mongo/db/cst/pipeline_grammar.yy"
