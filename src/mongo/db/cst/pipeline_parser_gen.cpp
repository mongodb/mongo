// A Bison parser, made by GNU Bison 3.6.

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

#line 63 "src/mongo/db/cst/pipeline_parser_gen.cpp"


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

#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
namespace mongo {
#line 156 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#if YYDEBUG || 0
const char* PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytname_[yysymbol];
}
#endif  // #if YYDEBUG || 0


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
        case 53:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 60:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 62:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 59:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 58:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 61:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

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
        case 93:   // value
        case 94:   // string
        case 95:   // binary
        case 96:   // undefined
        case 97:   // objectId
        case 98:   // bool
        case 99:   // date
        case 100:  // null
        case 101:  // regex
        case 102:  // simpleValue
        case 103:  // compoundValue
        case 104:  // valueArray
        case 105:  // valueObject
        case 106:  // valueFields
        case 107:  // stageList
        case 108:  // stage
        case 109:  // inhibitOptimization
        case 110:  // unionWith
        case 111:  // skip
        case 112:  // limit
        case 113:  // project
        case 114:  // sample
        case 115:  // projectFields
        case 116:  // projection
        case 117:  // num
        case 118:  // expression
        case 119:  // compoundExpression
        case 120:  // exprFixedTwoArg
        case 121:  // expressionArray
        case 122:  // expressionObject
        case 123:  // expressionFields
        case 124:  // maths
        case 125:  // add
        case 126:  // atan2
        case 127:  // boolExps
        case 128:  // and
        case 129:  // or
        case 130:  // not
        case 131:  // literalEscapes
        case 132:  // const
        case 133:  // literal
        case 134:  // compExprs
        case 135:  // cmp
        case 136:  // eq
        case 137:  // gt
        case 138:  // gte
        case 139:  // lt
        case 140:  // lte
        case 141:  // ne
        case 142:  // typeExpression
        case 143:  // typeValue
        case 144:  // convert
        case 145:  // toBool
        case 146:  // toDate
        case 147:  // toDecimal
        case 148:  // toDouble
        case 149:  // toInt
        case 150:  // toLong
        case 151:  // toObjectId
        case 152:  // toString
        case 153:  // type
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 71:  // projectionFieldname
        case 72:  // expressionFieldname
        case 73:  // stageAsUserFieldname
        case 74:  // argAsUserFieldname
        case 75:  // aggExprAsUserFieldname
        case 76:  // invariableUserFieldname
        case 77:  // idAsUserFieldname
        case 78:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 56:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 67:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 55:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 64:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 69:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 68:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 57:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 54:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 66:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 63:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 65:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 79:   // projectField
        case 80:   // expressionField
        case 81:   // valueField
        case 154:  // onErrorArg
        case 155:  // onNullArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 51:  // FIELDNAME
        case 52:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 156:  // expressions
        case 157:  // values
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
        case 53:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 60:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 62:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 59:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 58:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 61:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

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
        case 93:   // value
        case 94:   // string
        case 95:   // binary
        case 96:   // undefined
        case 97:   // objectId
        case 98:   // bool
        case 99:   // date
        case 100:  // null
        case 101:  // regex
        case 102:  // simpleValue
        case 103:  // compoundValue
        case 104:  // valueArray
        case 105:  // valueObject
        case 106:  // valueFields
        case 107:  // stageList
        case 108:  // stage
        case 109:  // inhibitOptimization
        case 110:  // unionWith
        case 111:  // skip
        case 112:  // limit
        case 113:  // project
        case 114:  // sample
        case 115:  // projectFields
        case 116:  // projection
        case 117:  // num
        case 118:  // expression
        case 119:  // compoundExpression
        case 120:  // exprFixedTwoArg
        case 121:  // expressionArray
        case 122:  // expressionObject
        case 123:  // expressionFields
        case 124:  // maths
        case 125:  // add
        case 126:  // atan2
        case 127:  // boolExps
        case 128:  // and
        case 129:  // or
        case 130:  // not
        case 131:  // literalEscapes
        case 132:  // const
        case 133:  // literal
        case 134:  // compExprs
        case 135:  // cmp
        case 136:  // eq
        case 137:  // gt
        case 138:  // gte
        case 139:  // lt
        case 140:  // lte
        case 141:  // ne
        case 142:  // typeExpression
        case 143:  // typeValue
        case 144:  // convert
        case 145:  // toBool
        case 146:  // toDate
        case 147:  // toDecimal
        case 148:  // toDouble
        case 149:  // toInt
        case 150:  // toLong
        case 151:  // toObjectId
        case 152:  // toString
        case 153:  // type
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 71:  // projectionFieldname
        case 72:  // expressionFieldname
        case 73:  // stageAsUserFieldname
        case 74:  // argAsUserFieldname
        case 75:  // aggExprAsUserFieldname
        case 76:  // invariableUserFieldname
        case 77:  // idAsUserFieldname
        case 78:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 56:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 67:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 55:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 64:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 69:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 68:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 57:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 54:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 66:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 63:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 65:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 79:   // projectField
        case 80:   // expressionField
        case 81:   // valueField
        case 154:  // onErrorArg
        case 155:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 51:  // FIELDNAME
        case 52:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 156:  // expressions
        case 157:  // values
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
        case 53:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 60:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 62:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 59:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 58:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 61:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

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
        case 93:   // value
        case 94:   // string
        case 95:   // binary
        case 96:   // undefined
        case 97:   // objectId
        case 98:   // bool
        case 99:   // date
        case 100:  // null
        case 101:  // regex
        case 102:  // simpleValue
        case 103:  // compoundValue
        case 104:  // valueArray
        case 105:  // valueObject
        case 106:  // valueFields
        case 107:  // stageList
        case 108:  // stage
        case 109:  // inhibitOptimization
        case 110:  // unionWith
        case 111:  // skip
        case 112:  // limit
        case 113:  // project
        case 114:  // sample
        case 115:  // projectFields
        case 116:  // projection
        case 117:  // num
        case 118:  // expression
        case 119:  // compoundExpression
        case 120:  // exprFixedTwoArg
        case 121:  // expressionArray
        case 122:  // expressionObject
        case 123:  // expressionFields
        case 124:  // maths
        case 125:  // add
        case 126:  // atan2
        case 127:  // boolExps
        case 128:  // and
        case 129:  // or
        case 130:  // not
        case 131:  // literalEscapes
        case 132:  // const
        case 133:  // literal
        case 134:  // compExprs
        case 135:  // cmp
        case 136:  // eq
        case 137:  // gt
        case 138:  // gte
        case 139:  // lt
        case 140:  // lte
        case 141:  // ne
        case 142:  // typeExpression
        case 143:  // typeValue
        case 144:  // convert
        case 145:  // toBool
        case 146:  // toDate
        case 147:  // toDecimal
        case 148:  // toDouble
        case 149:  // toInt
        case 150:  // toLong
        case 151:  // toObjectId
        case 152:  // toString
        case 153:  // type
            value.copy<CNode>(that.value);
            break;

        case 71:  // projectionFieldname
        case 72:  // expressionFieldname
        case 73:  // stageAsUserFieldname
        case 74:  // argAsUserFieldname
        case 75:  // aggExprAsUserFieldname
        case 76:  // invariableUserFieldname
        case 77:  // idAsUserFieldname
        case 78:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 56:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 67:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 55:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 64:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 69:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 68:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 57:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 54:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 66:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 63:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 65:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 79:   // projectField
        case 80:   // expressionField
        case 81:   // valueField
        case 154:  // onErrorArg
        case 155:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 51:  // FIELDNAME
        case 52:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 156:  // expressions
        case 157:  // values
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
        case 53:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 60:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 62:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 59:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 58:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 61:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

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
        case 93:   // value
        case 94:   // string
        case 95:   // binary
        case 96:   // undefined
        case 97:   // objectId
        case 98:   // bool
        case 99:   // date
        case 100:  // null
        case 101:  // regex
        case 102:  // simpleValue
        case 103:  // compoundValue
        case 104:  // valueArray
        case 105:  // valueObject
        case 106:  // valueFields
        case 107:  // stageList
        case 108:  // stage
        case 109:  // inhibitOptimization
        case 110:  // unionWith
        case 111:  // skip
        case 112:  // limit
        case 113:  // project
        case 114:  // sample
        case 115:  // projectFields
        case 116:  // projection
        case 117:  // num
        case 118:  // expression
        case 119:  // compoundExpression
        case 120:  // exprFixedTwoArg
        case 121:  // expressionArray
        case 122:  // expressionObject
        case 123:  // expressionFields
        case 124:  // maths
        case 125:  // add
        case 126:  // atan2
        case 127:  // boolExps
        case 128:  // and
        case 129:  // or
        case 130:  // not
        case 131:  // literalEscapes
        case 132:  // const
        case 133:  // literal
        case 134:  // compExprs
        case 135:  // cmp
        case 136:  // eq
        case 137:  // gt
        case 138:  // gte
        case 139:  // lt
        case 140:  // lte
        case 141:  // ne
        case 142:  // typeExpression
        case 143:  // typeValue
        case 144:  // convert
        case 145:  // toBool
        case 146:  // toDate
        case 147:  // toDecimal
        case 148:  // toDouble
        case 149:  // toInt
        case 150:  // toLong
        case 151:  // toObjectId
        case 152:  // toString
        case 153:  // type
            value.move<CNode>(that.value);
            break;

        case 71:  // projectionFieldname
        case 72:  // expressionFieldname
        case 73:  // stageAsUserFieldname
        case 74:  // argAsUserFieldname
        case 75:  // aggExprAsUserFieldname
        case 76:  // invariableUserFieldname
        case 77:  // idAsUserFieldname
        case 78:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 56:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 67:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 55:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 64:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 69:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 68:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 57:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 54:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 66:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 63:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 65:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 79:   // projectField
        case 80:   // expressionField
        case 81:   // valueField
        case 154:  // onErrorArg
        case 155:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 51:  // FIELDNAME
        case 52:  // STRING
            value.move<std::string>(that.value);
            break;

        case 156:  // expressions
        case 157:  // values
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
        yyo << (yykind < YYNTOKENS ? "token" : "nterm") << ' ' << symbol_name(yykind) << " ("
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
                case 53:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 60:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 62:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 59:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 58:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 61:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

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
                case 93:   // value
                case 94:   // string
                case 95:   // binary
                case 96:   // undefined
                case 97:   // objectId
                case 98:   // bool
                case 99:   // date
                case 100:  // null
                case 101:  // regex
                case 102:  // simpleValue
                case 103:  // compoundValue
                case 104:  // valueArray
                case 105:  // valueObject
                case 106:  // valueFields
                case 107:  // stageList
                case 108:  // stage
                case 109:  // inhibitOptimization
                case 110:  // unionWith
                case 111:  // skip
                case 112:  // limit
                case 113:  // project
                case 114:  // sample
                case 115:  // projectFields
                case 116:  // projection
                case 117:  // num
                case 118:  // expression
                case 119:  // compoundExpression
                case 120:  // exprFixedTwoArg
                case 121:  // expressionArray
                case 122:  // expressionObject
                case 123:  // expressionFields
                case 124:  // maths
                case 125:  // add
                case 126:  // atan2
                case 127:  // boolExps
                case 128:  // and
                case 129:  // or
                case 130:  // not
                case 131:  // literalEscapes
                case 132:  // const
                case 133:  // literal
                case 134:  // compExprs
                case 135:  // cmp
                case 136:  // eq
                case 137:  // gt
                case 138:  // gte
                case 139:  // lt
                case 140:  // lte
                case 141:  // ne
                case 142:  // typeExpression
                case 143:  // typeValue
                case 144:  // convert
                case 145:  // toBool
                case 146:  // toDate
                case 147:  // toDecimal
                case 148:  // toDouble
                case 149:  // toInt
                case 150:  // toLong
                case 151:  // toObjectId
                case 152:  // toString
                case 153:  // type
                    yylhs.value.emplace<CNode>();
                    break;

                case 71:  // projectionFieldname
                case 72:  // expressionFieldname
                case 73:  // stageAsUserFieldname
                case 74:  // argAsUserFieldname
                case 75:  // aggExprAsUserFieldname
                case 76:  // invariableUserFieldname
                case 77:  // idAsUserFieldname
                case 78:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 56:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 67:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 55:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 64:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 69:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 68:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 57:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 54:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 66:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 63:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 65:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 79:   // projectField
                case 80:   // expressionField
                case 81:   // valueField
                case 154:  // onErrorArg
                case 155:  // onNullArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 51:  // FIELDNAME
                case 52:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 156:  // expressions
                case 157:  // values
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
#line 230 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 3:
#line 236 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 237 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1436 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 245 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1442 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1448 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 8:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1454 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1460 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1466 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1472 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 248 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1478 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 251 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1490 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 261 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1498 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 267 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1511 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 277 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1517 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 277 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1523 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 277 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1529 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 277 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1535 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 281 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1543 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1551 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 291 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1559 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 297 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1567 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 300 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1576 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 307 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1584 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1592 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 316 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1598 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 317 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1606 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 320 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1614 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 323 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1622 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 326 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1630 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 329 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1638 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 332 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1646 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 335 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1654 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 338 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1662 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1670 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 344 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1678 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 347 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1684 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1690 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1696 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1702 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1708 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 355 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 363 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1724 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 366 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1732 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 369 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 1740 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 372 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 1748 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 375 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 1756 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 378 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 1764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 387 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 1772 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 1780 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 393 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 1788 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 396 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 1796 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 399 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 1804 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 402 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 1812 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 405 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 1820 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 413 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 1828 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 416 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 1836 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 419 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 1844 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 422 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 1852 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 1860 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 428 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 1868 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 431 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 1876 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 434 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 1884 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 437 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 1892 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 440 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 1900 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 443 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 1908 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 446 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 1916 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 449 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 1924 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 452 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 1932 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 455 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 1940 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 1948 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 461 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 1956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 464 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 1964 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 467 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 1972 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 470 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 1980 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 473 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 1988 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 476 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 1996 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 479 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2004 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 482 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2012 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 489 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2020 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 495 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2028 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 501 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2036 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 507 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2044 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 513 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2052 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 519 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2060 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 525 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2068 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 531 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2076 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 537 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2084 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 543 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2092 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 549 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2100 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 555 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2108 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 561 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2116 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 567 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2124 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 573 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2132 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 576 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2140 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 582 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2148 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 585 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2156 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 591 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2164 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 594 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2172 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 600 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2180 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 603 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2188 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 609 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2196 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 612 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2204 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 618 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2210 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 619 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2216 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 620 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2222 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 621 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2228 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 622 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2234 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 623 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2240 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 624 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2246 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 625 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2252 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 626 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2258 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 627 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2264 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 628 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2270 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 629 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2276 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 630 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2282 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 631 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2288 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 632 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2294 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 633 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2300 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 634 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2306 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 635 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2312 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 636 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2318 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 643 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2324 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 644 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2333 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 651 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2339 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 651 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2345 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 655 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2353 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2359 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2365 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2371 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2377 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 660 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2389 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 661 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2395 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 667 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2403 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 675 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2411 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 681 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2419 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 684 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2428 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 691 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2436 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2442 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2448 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2454 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 698 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2460 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 702 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2468 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 708 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2474 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 709 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2480 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 713 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2492 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 723 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2501 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 730 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2507 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 730 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2513 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 730 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2519 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 734 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2531 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 744 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 2543 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 754 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2552 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 761 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2558 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 761 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2564 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 765 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2573 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 772 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2582 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 779 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2588 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 779 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2594 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 783 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2600 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 783 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2606 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 787 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2614 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 793 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2620 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 794 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2629 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 801 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2637 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 807 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2645 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 810 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2654 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 817 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2662 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 824 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2668 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 825 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2674 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 826 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2680 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 827 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2686 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 828 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2692 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2698 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2704 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2710 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2722 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2728 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 831 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2734 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 833 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2743 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 838 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2752 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 843 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2761 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 848 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2770 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 853 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2779 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 858 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2788 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 863 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2797 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 869 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2803 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 870 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2809 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 871 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2815 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 872 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2821 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 873 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2827 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 874 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2833 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 875 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2839 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 876 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2845 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 877 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2851 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 878 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2857 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2863 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2869 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2875 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 884 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2887 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 888 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 2895 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 891 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2903 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 898 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 2911 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 901 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2919 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 907 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::convert,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::toArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 2930 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 916 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2938 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 921 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2946 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 926 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2954 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 931 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2962 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 936 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2970 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 941 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2978 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 946 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2986 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 951 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2994 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 956 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3002 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 3006 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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
        if (!yyla.empty())                                       // NOLINT(bugprone-use-after-move)
            yy_destroy_("Cleanup: discarding lookahead", yyla);  // NOLINT(bugprone-use-after-move)

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


const short PipelineParserGen::yypact_ninf_ = -190;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    29,   35,   39,   59,   34,   -190, 46,   48,   49,   50,   48,   -190, 47,   -190, -190, -190,
    -190, -190, -190, -190, 51,   -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, 32,   -190, 43,   63,   35,   -190, 297,  48,   17,   -190, -190, -190, 167,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, 167,  -190, -190, -190, -190, -190, 66,   -190, 58,   374,
    129,  -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, 15,
    75,   77,   79,   84,   85,   87,   89,   77,   77,   77,   77,   77,   77,   77,   -190, 129,
    129,  129,  129,  129,  129,  129,  129,  129,  222,  -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, 129,  -190, 90,   95,
    129,  129,  98,   129,  147,  147,  129,  129,  99,   100,  101,  102,  104,  108,  112,  70,
    116,  118,  119,  120,  121,  122,  123,  124,  125,  -190, -190, 129,  -190, -190, -190, -190,
    -190, -190, -190, -190, 129,  129,  -190, 129,  -190, 147,  137,  -190, -190, -190, -190, 138,
    129,  139,  -190, -190, -190, -190, -190, -190, -190, 129,  -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, 129,  140,  129,  345,  147,  141,  126,  127,  129,  131,  103,  142,
    -190, 143,  -190, -190, -190, -190, -190, -190, 147,  -190, -190, -190, -190, -190, 148,  -190,
    20,   149,  157,  -190, 158,  -190, -190, -190, -190, -190, 114,  -190, -190, -190, 129,  115,
    -190, 129,  160,  -190, 162,  -190};

const unsigned char PipelineParserGen::yydefact_[] = {
    0,   3,   0,   0,   0,   1,   0,   0,   0,   0,   0,   5,   0,   7,   8,   9,   10,  11,  12,
    2,   0,   96,  98,  100, 102, 95,  97,  99,  101, 16,  17,  18,  19,  21,  23,  0,   20,  0,
    0,   3,   14,  0,   0,   0,   6,   4,   22,  0,   44,  47,  48,  49,  46,  45,  50,  51,  52,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,
    76,  77,  78,  79,  80,  53,  54,  55,  56,  43,  0,   40,  41,  42,  39,  24,  0,   81,  0,
    138, 124, 29,  31,  33,  35,  36,  37,  28,  30,  32,  34,  27,  25,  38,  129, 130, 131, 146,
    147, 132, 150, 151, 152, 133, 156, 157, 134, 176, 177, 178, 179, 180, 181, 182, 135, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 26,  13,  0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   5,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   103, 104,
    82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  112, 113, 114, 115, 116, 121,
    117, 118, 119, 122, 123, 105, 106, 107, 108, 120, 109, 110, 111, 126, 124, 127, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   137, 145, 0,   142, 143, 141, 144, 139, 125, 136, 15,  0,   0,   149,
    0,   168, 165, 0,   160, 161, 162, 163, 0,   0,   0,   183, 184, 185, 186, 187, 188, 189, 0,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 140, 124, 0,   124, 0,   165, 0,   0,   0,   124,
    0,   0,   0,   128, 0,   167, 172, 173, 174, 171, 175, 0,   169, 166, 164, 158, 159, 0,   155,
    0,   0,   0,   170, 0,   201, 202, 203, 204, 200, 205, 148, 153, 154, 0,   207, 206, 0,   0,
    208, 0,   209};

const short PipelineParserGen::yypgoto_[] = {
    -190, -190, -190, -160, -158, -112, -146, -111, -190, -190, -190, -190, -190, -190, -190, -190,
    -6,   -190, -5,   -7,   -1,   -190, -190, -186, -36,  -190, -190, -190, -190, -190, -190, -190,
    -189, -190, -190, -190, -190, 130,  -190, -190, -190, -190, -190, -190, -190, -190, 132,  23,
    -147, -15,  -109, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190, -190,
    -190, -190, -190, -190, -190, -190, -183, -107, -190, 62,   -190};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  86,  235, 87,  88,  89,  90,  239, 296, 91,  240, 297, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 280, 195, 196, 197, 198, 199, 200, 201, 202, 203, 252, 253, 254, 279, 4,
    12,  13,  14,  15,  16,  17,  18,  41,  108, 33,  204, 205, 210, 110, 111, 168, 112, 113, 114,
    115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 314, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 140, 319, 322, 206, 281, 2,   37,  38};

const short PipelineParserGen::yytable_[] = {
    31,  29,  30,  31,  29,  30,  32,  94,  236, 32,  237, 107, 224, 225, 226, 227, 228, 229, 230,
    231, 232, 241, 238, 251, 251, 23,  250, 255, 21,  22,  23,  24,  109, 36,  1,   31,  29,  30,
    3,   5,   19,  32,  216, 217, 218, 219, 220, 221, 222, 20,  107, 39,  34,  35,  42,  40,  21,
    22,  23,  24,  251, 244, 245, 43,  247, 92,  44,  256, 257, 93,  142, 109, 93,  6,   7,   8,
    9,   10,  11,  143, 208, 27,  209, 25,  211, 26,  27,  28,  275, 212, 213, 251, 214, 287, 215,
    289, 242, 276, 277, 243, 278, 302, 246, 258, 259, 260, 261, 251, 262, 284, 307, 25,  263, 26,
    27,  28,  264, 265, 286, 291, 266, 292, 267, 268, 269, 270, 271, 272, 273, 274, 300, 301, 95,
    294, 96,  303, 207, 21,  22,  23,  24,  169, 170, 282, 283, 285, 288, 299, 305, 306, 248, 304,
    249, 315, 308, 21,  22,  23,  24,  169, 170, 316, 317, 318, 324, 321, 325, 293, 295, 45,  95,
    320, 96,  298, 323, 97,  98,  99,  100, 101, 102, 93,  171, 172, 173, 174, 175, 176, 177, 178,
    179, 180, 25,  181, 26,  27,  28,  182, 183, 93,  171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 25,  181, 26,  27,  28,  182, 183, 0,   141, 93,  223, 0,   0,   0,   0,   0,   233, 0,
    0,   234, 103, 0,   104, 105, 106, 0,   48,  49,  50,  51,  52,  53,  54,  55,  56,  0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   313, 81,  82,  83,  84,  85,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   311, 309, 310, 0,   46,  0,   312,
    47,  0,   0,   0,   0,   0,   0,   48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,
    79,  80,  81,  82,  83,  84,  85,  290, 0,   0,   234, 0,   0,   0,   0,   0,   0,   48,  49,
    50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,
    69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  144, 145,
    146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167};

const short PipelineParserGen::yycheck_[] = {
    7,   7,   7,   10,  10,  10,  7,   43,  168, 10,  168, 47,  159, 160, 161, 162, 163, 164, 165,
    166, 167, 204, 168, 212, 213, 10,  212, 213, 8,   9,   10,  11,  47,  10,  5,   42,  42,  42,
    3,   0,   6,   42,  151, 152, 153, 154, 155, 156, 157, 3,   86,  4,   3,   3,   22,  4,   8,
    9,   10,  11,  249, 208, 209, 20,  211, 42,  3,   214, 215, 52,  4,   86,  52,  14,  15,  16,
    17,  18,  19,  21,  5,   66,  5,   63,  5,   65,  66,  67,  235, 5,   5,   280, 5,   276, 5,
    278, 6,   244, 245, 4,   247, 284, 4,   4,   4,   4,   4,   296, 4,   256, 296, 63,  4,   65,
    66,  67,  4,   47,  265, 279, 4,   279, 4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   3,
    279, 5,   4,   143, 8,   9,   10,  11,  12,  13,  6,   6,   6,   6,   6,   6,   6,   3,   48,
    5,   4,   6,   8,   9,   10,  11,  12,  13,  4,   4,   49,  4,   50,  4,   279, 279, 39,  3,
    318, 5,   280, 321, 8,   9,   10,  11,  12,  13,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  69,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  69,  -1,  86,  52,  158, -1,  -1,  -1,  -1,  -1,  4,   -1,
    -1,  7,   63,  -1,  65,  66,  67,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  304, 47,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  304, 304, 304, -1,  4,   -1,  304,
    7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,
    16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   5,   158, 3,   107, 0,   14,  15,  16,  17,  18,  19,  108, 109, 110, 111, 112, 113, 114,
    6,   3,   8,   9,   10,  11,  63,  65,  66,  67,  86,  88,  89,  90,  117, 3,   3,   117, 159,
    160, 4,   4,   115, 22,  20,  3,   107, 4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  71,  73,  74,  75,  76,  79,  117, 52,  94,
    3,   5,   8,   9,   10,  11,  12,  13,  63,  65,  66,  67,  94,  116, 119, 121, 122, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 144, 145,
    146, 147, 148, 149, 150, 151, 152, 153, 116, 4,   21,  23,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  123, 12,  13,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  64,  68,  69,  82,  83,  84,  85,  86,  87,
    88,  89,  90,  91,  92,  94,  95,  96,  97,  98,  99,  100, 101, 102, 118, 119, 156, 89,  5,
    5,   120, 5,   5,   5,   5,   5,   120, 120, 120, 120, 120, 120, 120, 159, 118, 118, 118, 118,
    118, 118, 118, 118, 118, 4,   7,   72,  73,  74,  76,  77,  80,  156, 6,   4,   118, 118, 4,
    118, 3,   5,   93,  102, 103, 104, 105, 93,  118, 118, 4,   4,   4,   4,   4,   4,   4,   47,
    4,   4,   4,   4,   4,   4,   4,   4,   4,   118, 118, 118, 118, 106, 93,  157, 6,   6,   118,
    6,   118, 156, 6,   156, 4,   73,  74,  75,  76,  77,  78,  81,  157, 6,   4,   4,   156, 4,
    48,  6,   6,   93,  6,   86,  88,  89,  90,  94,  143, 4,   4,   4,   49,  154, 118, 50,  155,
    118, 4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   70,  158, 107, 107, 160, 159, 108, 108, 108, 108, 108, 108, 114, 109, 110, 117, 117, 117,
    117, 111, 112, 113, 115, 115, 79,  79,  116, 116, 116, 116, 116, 116, 116, 116, 116, 116, 116,
    116, 71,  71,  71,  71,  76,  73,  73,  73,  73,  73,  73,  74,  74,  74,  74,  74,  74,  74,
    75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,  75,
    75,  75,  75,  75,  75,  94,  95,  96,  97,  99,  100, 101, 82,  83,  84,  85,  87,  91,  92,
    86,  86,  88,  88,  89,  89,  90,  90,  98,  98,  102, 102, 102, 102, 102, 102, 102, 102, 102,
    102, 102, 102, 102, 102, 102, 102, 102, 102, 102, 156, 156, 118, 118, 120, 119, 119, 119, 119,
    119, 119, 119, 121, 122, 123, 123, 80,  72,  72,  72,  72,  77,  124, 124, 125, 126, 127, 127,
    127, 128, 129, 130, 131, 131, 132, 133, 93,  93,  103, 103, 104, 157, 157, 105, 106, 106, 81,
    78,  78,  78,  78,  78,  134, 134, 134, 134, 134, 134, 134, 135, 136, 137, 138, 139, 140, 141,
    142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 143, 143, 143, 143, 143, 154, 154, 155, 155,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7, 1, 1,  1, 1, 2, 2, 4, 0, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1,
    4, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1,  1, 1, 8, 4, 1, 1, 1, 8, 8, 6, 1, 1, 6, 6,
    1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "CONVERT",
                                                   "TO_BOOL",
                                                   "TO_DATE",
                                                   "TO_DECIMAL",
                                                   "TO_DOUBLE",
                                                   "TO_INT",
                                                   "TO_LONG",
                                                   "TO_OBJECT_ID",
                                                   "TO_STRING",
                                                   "TYPE",
                                                   "INPUT_ARG",
                                                   "TO_ARG",
                                                   "ON_ERROR_ARG",
                                                   "ON_NULL_ARG",
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
                                                   "typeExpression",
                                                   "typeValue",
                                                   "convert",
                                                   "toBool",
                                                   "toDate",
                                                   "toDecimal",
                                                   "toDouble",
                                                   "toInt",
                                                   "toLong",
                                                   "toObjectId",
                                                   "toString",
                                                   "type",
                                                   "onErrorArg",
                                                   "onNullArg",
                                                   "expressions",
                                                   "values",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,   230, 230, 236, 237, 245, 245, 248, 248, 248, 248, 248, 248, 251, 261, 267, 277, 277, 277,
    277, 281, 286, 291, 297, 300, 307, 310, 316, 317, 320, 323, 326, 329, 332, 335, 338, 341, 344,
    347, 351, 351, 351, 351, 355, 363, 366, 369, 372, 375, 378, 387, 390, 393, 396, 399, 402, 405,
    413, 416, 419, 422, 425, 428, 431, 434, 437, 440, 443, 446, 449, 452, 455, 458, 461, 464, 467,
    470, 473, 476, 479, 482, 489, 495, 501, 507, 513, 519, 525, 531, 537, 543, 549, 555, 561, 567,
    573, 576, 582, 585, 591, 594, 600, 603, 609, 612, 618, 619, 620, 621, 622, 623, 624, 625, 626,
    627, 628, 629, 630, 631, 632, 633, 634, 635, 636, 643, 644, 651, 651, 655, 660, 660, 660, 660,
    660, 660, 661, 667, 675, 681, 684, 691, 698, 698, 698, 698, 702, 708, 709, 713, 723, 730, 730,
    730, 734, 744, 754, 761, 761, 765, 772, 779, 779, 783, 783, 787, 793, 794, 801, 807, 810, 817,
    824, 825, 826, 827, 828, 831, 831, 831, 831, 831, 831, 831, 833, 838, 843, 848, 853, 858, 863,
    869, 870, 871, 872, 873, 874, 875, 876, 877, 878, 884, 884, 884, 884, 884, 888, 891, 898, 901,
    907, 916, 921, 926, 931, 936, 941, 946, 951, 956};

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


#line 58 "src/mongo/db/cst/pipeline_grammar.yy"
}  // namespace mongo
#line 3582 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 960 "src/mongo/db/cst/pipeline_grammar.yy"
