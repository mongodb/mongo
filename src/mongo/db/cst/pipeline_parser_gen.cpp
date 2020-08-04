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
#line 81 "pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse,
              str::stream() << msg << " at location " << loc.begin.line << ":" << loc.begin.column
                            << " of input BSON. Lexer produced token of type "
                            << lexer[loc.begin.column].type_get() << ".");
}
}  // namespace mongo

#line 66 "pipeline_parser_gen.cpp"


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
#line 159 "pipeline_parser_gen.cpp"

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
        case 68:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 75:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 74:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 73:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 76:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 71:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 70:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 79:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 84:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 83:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 72:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 69:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 78:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 80:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 190:  // expressions
        case 191:  // values
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
        case 68:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 75:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 74:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 73:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 76:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 71:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 70:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 79:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 84:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 83:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 72:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 69:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 78:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 80:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 190:  // expressions
        case 191:  // values
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
        case 68:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 75:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 74:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 73:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 76:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 71:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 70:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 79:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 84:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 83:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 72:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 69:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 78:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 80:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.copy<std::string>(that.value);
            break;

        case 190:  // expressions
        case 191:  // values
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
        case 68:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 75:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 77:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 74:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 73:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 76:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 101:  // dbPointer
        case 102:  // javascript
        case 103:  // symbol
        case 104:  // javascriptWScope
        case 105:  // int
        case 106:  // timestamp
        case 107:  // long
        case 108:  // double
        case 109:  // decimal
        case 110:  // minKey
        case 111:  // maxKey
        case 112:  // value
        case 113:  // string
        case 114:  // binary
        case 115:  // undefined
        case 116:  // objectId
        case 117:  // bool
        case 118:  // date
        case 119:  // null
        case 120:  // regex
        case 121:  // simpleValue
        case 122:  // compoundValue
        case 123:  // valueArray
        case 124:  // valueObject
        case 125:  // valueFields
        case 126:  // stageList
        case 127:  // stage
        case 128:  // inhibitOptimization
        case 129:  // unionWith
        case 130:  // skip
        case 131:  // limit
        case 132:  // project
        case 133:  // sample
        case 134:  // projectFields
        case 135:  // projection
        case 136:  // num
        case 137:  // expression
        case 138:  // compoundExpression
        case 139:  // exprFixedTwoArg
        case 140:  // expressionArray
        case 141:  // expressionObject
        case 142:  // expressionFields
        case 143:  // maths
        case 144:  // add
        case 145:  // atan2
        case 146:  // boolExps
        case 147:  // and
        case 148:  // or
        case 149:  // not
        case 150:  // literalEscapes
        case 151:  // const
        case 152:  // literal
        case 153:  // compExprs
        case 154:  // cmp
        case 155:  // eq
        case 156:  // gt
        case 157:  // gte
        case 158:  // lt
        case 159:  // lte
        case 160:  // ne
        case 161:  // typeExpression
        case 162:  // typeValue
        case 163:  // convert
        case 164:  // toBool
        case 165:  // toDate
        case 166:  // toDecimal
        case 167:  // toDouble
        case 168:  // toInt
        case 169:  // toLong
        case 170:  // toObjectId
        case 171:  // toString
        case 172:  // type
        case 173:  // abs
        case 174:  // ceil
        case 175:  // divide
        case 176:  // exponent
        case 177:  // floor
        case 178:  // ln
        case 179:  // log
        case 180:  // logten
        case 181:  // mod
        case 182:  // multiply
        case 183:  // pow
        case 184:  // round
        case 185:  // sqrt
        case 186:  // subtract
        case 187:  // trunc
        case 192:  // matchExpression
        case 193:  // filterFields
        case 194:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 88:  // projectionFieldname
        case 89:  // expressionFieldname
        case 90:  // stageAsUserFieldname
        case 91:  // filterFieldname
        case 92:  // argAsUserFieldname
        case 93:  // aggExprAsUserFieldname
        case 94:  // invariableUserFieldname
        case 95:  // idAsUserFieldname
        case 96:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 71:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 82:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 70:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 79:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 84:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 83:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 72:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 69:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 81:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 78:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 80:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 97:   // projectField
        case 98:   // expressionField
        case 99:   // valueField
        case 100:  // filterField
        case 188:  // onErrorArg
        case 189:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 66:  // FIELDNAME
        case 67:  // STRING
            value.move<std::string>(that.value);
            break;

        case 190:  // expressions
        case 191:  // values
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
                case 68:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 75:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 77:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 74:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 73:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 76:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 101:  // dbPointer
                case 102:  // javascript
                case 103:  // symbol
                case 104:  // javascriptWScope
                case 105:  // int
                case 106:  // timestamp
                case 107:  // long
                case 108:  // double
                case 109:  // decimal
                case 110:  // minKey
                case 111:  // maxKey
                case 112:  // value
                case 113:  // string
                case 114:  // binary
                case 115:  // undefined
                case 116:  // objectId
                case 117:  // bool
                case 118:  // date
                case 119:  // null
                case 120:  // regex
                case 121:  // simpleValue
                case 122:  // compoundValue
                case 123:  // valueArray
                case 124:  // valueObject
                case 125:  // valueFields
                case 126:  // stageList
                case 127:  // stage
                case 128:  // inhibitOptimization
                case 129:  // unionWith
                case 130:  // skip
                case 131:  // limit
                case 132:  // project
                case 133:  // sample
                case 134:  // projectFields
                case 135:  // projection
                case 136:  // num
                case 137:  // expression
                case 138:  // compoundExpression
                case 139:  // exprFixedTwoArg
                case 140:  // expressionArray
                case 141:  // expressionObject
                case 142:  // expressionFields
                case 143:  // maths
                case 144:  // add
                case 145:  // atan2
                case 146:  // boolExps
                case 147:  // and
                case 148:  // or
                case 149:  // not
                case 150:  // literalEscapes
                case 151:  // const
                case 152:  // literal
                case 153:  // compExprs
                case 154:  // cmp
                case 155:  // eq
                case 156:  // gt
                case 157:  // gte
                case 158:  // lt
                case 159:  // lte
                case 160:  // ne
                case 161:  // typeExpression
                case 162:  // typeValue
                case 163:  // convert
                case 164:  // toBool
                case 165:  // toDate
                case 166:  // toDecimal
                case 167:  // toDouble
                case 168:  // toInt
                case 169:  // toLong
                case 170:  // toObjectId
                case 171:  // toString
                case 172:  // type
                case 173:  // abs
                case 174:  // ceil
                case 175:  // divide
                case 176:  // exponent
                case 177:  // floor
                case 178:  // ln
                case 179:  // log
                case 180:  // logten
                case 181:  // mod
                case 182:  // multiply
                case 183:  // pow
                case 184:  // round
                case 185:  // sqrt
                case 186:  // subtract
                case 187:  // trunc
                case 192:  // matchExpression
                case 193:  // filterFields
                case 194:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 88:  // projectionFieldname
                case 89:  // expressionFieldname
                case 90:  // stageAsUserFieldname
                case 91:  // filterFieldname
                case 92:  // argAsUserFieldname
                case 93:  // aggExprAsUserFieldname
                case 94:  // invariableUserFieldname
                case 95:  // idAsUserFieldname
                case 96:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 71:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 82:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 70:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 79:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 84:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 83:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 72:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 69:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 81:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 78:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 80:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 97:   // projectField
                case 98:   // expressionField
                case 99:   // valueField
                case 100:  // filterField
                case 188:  // onErrorArg
                case 189:  // onNullArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 66:  // FIELDNAME
                case 67:  // STRING
                    yylhs.value.emplace<std::string>();
                    break;

                case 190:  // expressions
                case 191:  // values
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
                    case 3:
#line 250 "pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1516 "pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 257 "pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1524 "pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 263 "pipeline_grammar.yy"
                    {
                    }
#line 1530 "pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 264 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1538 "pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 272 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1544 "pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1550 "pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1556 "pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1562 "pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1568 "pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1574 "pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 275 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1580 "pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 278 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1592 "pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 288 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1600 "pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 294 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1613 "pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 304 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1619 "pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 304 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1625 "pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 304 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1631 "pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 304 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1637 "pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 308 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1645 "pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 313 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1653 "pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 318 "pipeline_grammar.yy"
                    {
                        auto&& fields = YY_MOVE(yystack_[1].value.as<CNode>());
                        if (auto inclusion =
                                c_node_validation::validateProjectionAsInclusionOrExclusion(fields);
                            inclusion.isOK())
                            yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                                inclusion.getValue() == c_node_validation::IsInclusion::yes
                                    ? KeyFieldname::projectInclusion
                                    : KeyFieldname::projectExclusion,
                                std::move(fields)}}};
                        else
                            // TODO SERVER-48810: Convert error string to Bison error with BSON
                            // location.
                            uassertStatusOK(inclusion);
                    }
#line 1671 "pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 334 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1679 "pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 337 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1688 "pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 344 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1696 "pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 347 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1704 "pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 353 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1710 "pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 354 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1716 "pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 355 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1722 "pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 356 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1728 "pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 357 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1734 "pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 358 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1740 "pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1746 "pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 360 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1752 "pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 361 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1758 "pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 362 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1764 "pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 363 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1770 "pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 364 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1778 "pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1786 "pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 370 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1794 "pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 373 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1802 "pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 376 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1810 "pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 379 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1818 "pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 382 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1826 "pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 385 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1834 "pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 388 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1842 "pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 391 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1850 "pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 394 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1856 "pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 395 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1862 "pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 396 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1868 "pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 397 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1874 "pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1880 "pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1886 "pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1892 "pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1898 "pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 405 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1906 "pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 411 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1914 "pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 414 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1923 "pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 421 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1931 "pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 424 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1939 "pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 430 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1945 "pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1951 "pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1957 "pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1963 "pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1969 "pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1977 "pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 446 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1985 "pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 449 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 1993 "pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 452 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2001 "pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 455 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2009 "pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 458 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2017 "pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 461 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2025 "pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 470 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2033 "pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 473 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2041 "pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 476 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2049 "pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 479 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2057 "pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 482 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2065 "pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2073 "pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 488 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2081 "pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 496 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2089 "pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 499 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2097 "pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 502 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2105 "pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 505 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2113 "pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 508 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2121 "pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 511 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2129 "pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 514 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2137 "pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 517 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2145 "pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 520 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2153 "pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 523 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2161 "pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 526 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2169 "pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 529 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2177 "pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 532 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2185 "pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 535 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2193 "pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 538 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2201 "pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 541 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2209 "pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 544 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2217 "pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 547 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2225 "pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 550 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2233 "pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 553 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2241 "pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 556 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2249 "pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 559 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2257 "pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 562 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2265 "pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 565 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2273 "pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 568 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2281 "pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 571 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2289 "pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 574 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2297 "pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 577 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2305 "pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 580 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2313 "pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 583 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2321 "pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 586 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2329 "pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 589 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2337 "pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 592 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2345 "pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 595 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2353 "pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 598 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2361 "pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 601 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2369 "pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 604 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2377 "pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 607 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2385 "pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 610 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2393 "pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 617 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2401 "pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 623 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2409 "pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 629 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2417 "pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 635 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2425 "pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 641 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2433 "pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 647 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2441 "pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 653 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2449 "pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 659 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2457 "pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 665 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2465 "pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 671 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2473 "pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 677 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2481 "pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 683 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2489 "pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 689 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2497 "pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 695 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2505 "pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 701 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2513 "pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 704 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2521 "pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 710 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2529 "pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 713 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2537 "pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 719 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2545 "pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 722 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2553 "pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 728 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2561 "pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 731 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2569 "pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 737 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2577 "pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 740 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2585 "pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 746 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2591 "pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 747 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2597 "pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 748 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2603 "pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 749 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2609 "pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 750 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2615 "pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 751 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2621 "pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 752 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2627 "pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 753 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2633 "pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 754 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2639 "pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 755 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2645 "pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 756 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2651 "pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 757 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2657 "pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 758 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2663 "pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 759 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2669 "pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 760 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2675 "pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 761 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2681 "pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 762 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2687 "pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 763 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2693 "pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 764 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2699 "pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 771 "pipeline_grammar.yy"
                    {
                    }
#line 2705 "pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 772 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2714 "pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 779 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2720 "pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 779 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2726 "pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 783 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2734 "pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2740 "pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2746 "pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2752 "pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2758 "pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2764 "pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 788 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2770 "pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 789 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2776 "pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 795 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2784 "pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 803 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2792 "pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 809 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2800 "pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 812 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2809 "pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 819 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2817 "pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 826 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2823 "pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 826 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2829 "pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 826 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2835 "pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 826 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2841 "pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 830 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2849 "pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2855 "pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2861 "pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2867 "pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2873 "pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2879 "pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2885 "pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2891 "pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2897 "pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2903 "pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2909 "pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2915 "pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2921 "pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 836 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2927 "pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 837 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2933 "pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 837 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2939 "pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 837 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2945 "pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 837 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2951 "pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 841 "pipeline_grammar.yy"
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
#line 2963 "pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 851 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2972 "pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 857 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2980 "pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 862 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2988 "pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 867 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 2997 "pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 873 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3005 "pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 878 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3013 "pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 883 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3021 "pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 888 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3030 "pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 894 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3038 "pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 899 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3047 "pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 905 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::multiply,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3059 "pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 914 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3068 "pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 920 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3077 "pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 926 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3085 "pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 931 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3094 "pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 937 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3103 "pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 943 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3109 "pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 943 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3115 "pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 943 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3121 "pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 947 "pipeline_grammar.yy"
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
#line 3133 "pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 957 "pipeline_grammar.yy"
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
#line 3145 "pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 967 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3154 "pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3160 "pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3166 "pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 978 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3175 "pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3184 "pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 992 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3190 "pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 992 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3196 "pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 996 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3202 "pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 996 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3208 "pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 1000 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3216 "pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 1006 "pipeline_grammar.yy"
                    {
                    }
#line 3222 "pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 1007 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3231 "pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 1014 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3239 "pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 1020 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3247 "pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 1023 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3256 "pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 1030 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3264 "pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1037 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3270 "pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1038 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3276 "pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1039 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3282 "pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1040 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3288 "pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1041 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3294 "pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3300 "pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3306 "pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3312 "pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3318 "pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3324 "pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3330 "pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3336 "pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1046 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3345 "pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1051 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3354 "pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1056 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3363 "pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1061 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3372 "pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1066 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3381 "pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1071 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3390 "pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1076 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3399 "pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1082 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3405 "pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1083 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3411 "pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1084 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3417 "pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1085 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3423 "pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1086 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3429 "pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1087 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3435 "pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1088 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3441 "pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1089 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3447 "pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1090 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3453 "pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1091 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3459 "pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3465 "pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3471 "pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3477 "pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3483 "pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1097 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3489 "pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1101 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 3497 "pipeline_parser_gen.cpp"
                    break;

                    case 276:
#line 1104 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3505 "pipeline_parser_gen.cpp"
                    break;

                    case 277:
#line 1111 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 3513 "pipeline_parser_gen.cpp"
                    break;

                    case 278:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3521 "pipeline_parser_gen.cpp"
                    break;

                    case 279:
#line 1120 "pipeline_grammar.yy"
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
#line 3532 "pipeline_parser_gen.cpp"
                    break;

                    case 280:
#line 1129 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3540 "pipeline_parser_gen.cpp"
                    break;

                    case 281:
#line 1134 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3548 "pipeline_parser_gen.cpp"
                    break;

                    case 282:
#line 1139 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3556 "pipeline_parser_gen.cpp"
                    break;

                    case 283:
#line 1144 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3564 "pipeline_parser_gen.cpp"
                    break;

                    case 284:
#line 1149 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3572 "pipeline_parser_gen.cpp"
                    break;

                    case 285:
#line 1154 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3580 "pipeline_parser_gen.cpp"
                    break;

                    case 286:
#line 1159 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3588 "pipeline_parser_gen.cpp"
                    break;

                    case 287:
#line 1164 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3596 "pipeline_parser_gen.cpp"
                    break;

                    case 288:
#line 1169 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3604 "pipeline_parser_gen.cpp"
                    break;


#line 3608 "pipeline_parser_gen.cpp"

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


const short PipelineParserGen::yypact_ninf_ = -278;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -64,  30,   44,   55,   58,   -278, -278, -278, -278, 66,   57,   405,  65,   124,  75,   89,
    124,  -278, 92,   -278, -278, -278, -278, -278, -278, -278, -278, 158,  -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, 158,  -278, -278, -278, -278, 94,   -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, 81,   -278, 86,   111,  58,   -278, 158,  -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, 468,  124,  48,   -278, -278, 531,  158,  115,
    -278, 178,  178,  -278, -278, -278, -278, -278, 120,  109,  -278, -278, -278, -278, -278, -278,
    -278, 158,  -278, -278, -278, 332,  265,  -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, 10,   -278, 131,  132,  134,  139,  140,  148,  151,  132,  132,  132,  132,  132,  132,
    132,  -278, 265,  265,  265,  265,  265,  265,  265,  265,  265,  265,  265,  157,  265,  265,
    265,  159,  265,  160,  167,  168,  169,  265,  170,  171,  288,  -278, 265,  -278, 172,  173,
    265,  265,  175,  265,  158,  158,  265,  265,  176,  181,  188,  189,  190,  192,  196,  141,
    197,  203,  204,  208,  210,  211,  212,  213,  214,  215,  216,  265,  217,  218,  219,  265,
    220,  265,  265,  265,  265,  239,  265,  265,  -278, 265,  -278, -278, -278, -278, -278, -278,
    -278, -278, 265,  265,  -278, 265,  257,  258,  265,  259,  -278, -278, -278, -278, -278, -278,
    -278, 265,  -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, 265,  -278, -278,
    -278, 265,  -278, 265,  265,  265,  265,  -278, 265,  265,  -278, 265,  260,  265,  263,  267,
    265,  275,  119,  274,  276,  277,  265,  278,  279,  280,  281,  284,  -278, 285,  -278, -278,
    287,  -278, 117,  290,  292,  293,  294,  295,  297,  307,  308,  309,  310,  311,  -278, -278,
    -278, -278, -278, 205,  -278, -278, -278, 312,  -278, -278, -278, -278, -278, -278, -278, 265,
    233,  -278, -278, 265,  313,  -278, 314,  -278};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   59,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,
    9,   10,  11,  12,  13,  14,  4,   58,  0,   69,  72,  73,  74,  71,  70,  75,  76,  77,  82,
    83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101,
    102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
    78,  79,  80,  81,  68,  65,  0,   66,  67,  64,  60,  0,   136, 138, 140, 142, 135, 137, 139,
    141, 18,  19,  20,  21,  23,  25,  0,   22,  0,   0,   5,   238, 235, 143, 144, 121, 122, 123,
    124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 152, 153, 154, 155, 156, 161, 157, 158,
    159, 162, 163, 63,  145, 146, 147, 148, 160, 149, 150, 151, 230, 231, 232, 233, 61,  62,  16,
    0,   0,   0,   8,   6,   0,   235, 0,   24,  0,   0,   55,  56,  57,  54,  26,  0,   0,   237,
    185, 242, 243, 244, 241, 245, 0,   239, 236, 234, 178, 164, 41,  43,  45,  47,  48,  49,  40,
    42,  44,  46,  36,  37,  38,  39,  50,  51,  52,  29,  30,  31,  32,  33,  34,  35,  27,  53,
    169, 170, 171, 186, 187, 172, 220, 221, 222, 173, 226, 227, 174, 246, 247, 248, 249, 250, 251,
    252, 175, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 188, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 28,  15,  0,   240, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   166, 164, 167, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   177, 0,   182, 183, 181, 184, 179, 165, 176, 17,  0,   0,   204, 0,   0,   0,   0,
    0,   253, 254, 255, 256, 257, 258, 259, 0,   280, 281, 282, 283, 284, 285, 286, 287, 288, 205,
    206, 0,   208, 209, 210, 0,   212, 0,   0,   0,   0,   217, 0,   0,   180, 164, 0,   164, 0,
    0,   164, 0,   0,   0,   0,   0,   164, 0,   0,   0,   0,   0,   168, 0,   228, 229, 0,   225,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   271, 272, 273, 274, 270, 275, 207,
    211, 213, 0,   215, 216, 218, 219, 203, 223, 224, 0,   277, 214, 276, 0,   0,   278, 0,   279};

const short PipelineParserGen::yypgoto_[] = {
    -278, -278, -278, -140, -278, -139, -101, -138, -17,  -278, -278, -278, -278, -278, -130, -128,
    -108, -103, -12,  -97,  -11,  -13,  -7,   -91,  -87,  -98,  -146, -85,  -75,  -72,  -278, -67,
    -53,  -51,  -20,  -278, -278, -278, -278, 221,  -278, -278, -278, -278, -278, -278, -278, -278,
    161,  -5,   -238, -49,  -242, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278, -278,
    -278, -278, -278, -278, -278, -278, -278, -277, 162,  -278, -278, 237,  -278, -278, 49,   -278};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  162, 345, 81,  82,  83,  84,  85,  176, 177, 167, 350, 178, 86,  125, 126, 127, 128, 129,
    130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 299, 146, 147, 148,
    157, 10,  18,  19,  20,  21,  22,  23,  24,  152, 207, 100, 300, 301, 306, 209, 210, 298, 211,
    212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 435,
    230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248,
    249, 250, 251, 252, 253, 254, 448, 452, 302, 159, 7,   11,  149, 3,   5,   104, 105};

const short PipelineParserGen::yytable_[] = {
    98,  96,  97,  98,  96,  97,  99,  145, 169, 99,  158, 103, 163, 164, 166, 200, 200, 172, 173,
    175, 90,  1,   2,   351, 312, 313, 314, 315, 316, 317, 318, 193, 193, 194, 194, 4,   320, 321,
    322, 323, 324, 325, 326, 327, 328, 329, 330, 6,   332, 333, 334, 165, 336, 195, 195, 8,   174,
    341, 196, 196, 158, 9,   145, 25,  197, 197, 354, 355, 87,  357, 198, 198, 360, 361, 199, 199,
    201, 201, 101, 258, 12,  13,  14,  15,  16,  17,  202, 202, 145, 203, 203, 94,  102, 381, 204,
    204, 106, 385, 151, 387, 388, 389, 390, 153, 392, 393, 154, 394, 205, 205, 206, 206, 208, 208,
    155, 111, 395, 396, 411, 397, 413, 180, 400, 416, 256, 88,  89,  90,  91,  422, 257, 402, 88,
    89,  90,  91,  304, 305, 145, 307, 98,  96,  97,  403, 308, 309, 99,  404, 168, 405, 406, 407,
    408, 310, 409, 410, 311, 145, 346, 347, 348, 107, 331, 108, 335, 337, 88,  89,  90,  91,  109,
    110, 338, 339, 340, 342, 343, 353, 352, 356, 362, 181, 418, 182, 111, 363, 183, 184, 185, 186,
    187, 188, 364, 365, 366, 92,  367, 93,  94,  95,  368, 370, 92,  369, 93,  94,  95,  371, 372,
    450, 358, 359, 373, 453, 374, 375, 376, 377, 378, 379, 380, 382, 383, 384, 386, 111, 112, 113,
    114, 115, 116, 117, 118, 119, 120, 121, 92,  122, 93,  94,  95,  123, 124, 391, 303, 111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 189, 122, 190, 191, 192, 123, 124, 398, 399, 401,
    412, 414, 181, 447, 182, 415, 434, 88,  89,  90,  91,  109, 110, 417, 419, 349, 420, 421, 423,
    424, 425, 426, 145, 145, 427, 428, 344, 429, 436, 171, 437, 438, 451, 440, 439, 441, 28,  29,
    30,  31,  32,  33,  34,  35,  36,  442, 443, 444, 445, 446, 449, 454, 455, 150, 179, 0,   319,
    255, 0,   0,   0,   156, 0,   0,   0,   0,   111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
    121, 92,  122, 93,  94,  95,  123, 124, 76,  77,  78,  79,  80,  259, 260, 261, 262, 263, 264,
    265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283,
    284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   432, 430, 431, 0,   26,  0,   433, 27,  0,   0,   0,   0,   0,
    0,   28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,
    46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  160, 0,   0,
    161, 0,   0,   0,   0,   0,   0,   28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  170, 0,   0,   171, 0,   0,   0,   0,   0,   0,   28,  29,  30,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,
    53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  79,  80};

const short PipelineParserGen::yycheck_[] = {
    13,  13,  13,  16,  16,  16,  13,  27,  154, 16,  108, 16,  152, 152, 152, 161, 162, 157, 157,
    157, 10,  85,  86,  300, 266, 267, 268, 269, 270, 271, 272, 161, 162, 161, 162, 5,   274, 275,
    276, 277, 278, 279, 280, 281, 282, 283, 284, 3,   286, 287, 288, 152, 290, 161, 162, 0,   157,
    295, 161, 162, 158, 3,   82,  6,   161, 162, 304, 305, 3,   307, 161, 162, 310, 311, 161, 162,
    161, 162, 3,   177, 14,  15,  16,  17,  18,  19,  161, 162, 108, 161, 162, 81,  3,   331, 161,
    162, 4,   335, 4,   337, 338, 339, 340, 22,  342, 343, 20,  345, 161, 162, 161, 162, 161, 162,
    3,   67,  354, 355, 395, 357, 397, 6,   360, 400, 4,   8,   9,   10,  11,  406, 21,  369, 8,
    9,   10,  11,  5,   5,   158, 5,   153, 153, 153, 381, 5,   5,   153, 385, 153, 387, 388, 389,
    390, 5,   392, 393, 5,   177, 298, 298, 298, 3,   5,   5,   5,   5,   8,   9,   10,  11,  12,
    13,  5,   5,   5,   5,   5,   4,   6,   4,   4,   3,   63,  5,   67,  4,   8,   9,   10,  11,
    12,  13,  4,   4,   4,   78,  4,   80,  81,  82,  4,   4,   78,  62,  80,  81,  82,  4,   4,
    447, 308, 309, 4,   451, 4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   67,  68,  69,
    70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  4,   257, 67,  68,
    69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  6,   6,   6,
    6,   4,   3,   64,  5,   4,   418, 8,   9,   10,  11,  12,  13,  4,   6,   298, 6,   6,   6,
    6,   6,   6,   308, 309, 6,   6,   4,   6,   4,   7,   4,   4,   65,  4,   6,   4,   14,  15,
    16,  17,  18,  19,  20,  21,  22,  4,   4,   4,   4,   4,   4,   4,   4,   82,  158, -1,  273,
    162, -1,  -1,  -1,  106, -1,  -1,  -1,  -1,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
    77,  78,  79,  80,  81,  82,  83,  84,  62,  63,  64,  65,  66,  23,  24,  25,  26,  27,  28,
    29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  418, 418, 418, -1,  4,   -1,  418, 7,   -1,  -1,  -1,  -1,  -1,
    -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
    51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  4,   -1,  -1,
    7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   85,  86,  195, 5,   196, 3,   192, 0,   3,   126, 193, 14,  15,  16,  17,  18,  19,  127,
    128, 129, 130, 131, 132, 133, 6,   4,   7,   14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  90,  91,  92,  93,  94,  100, 3,   8,   9,   10,  11,  78,  80,  81,
    82,  105, 107, 108, 109, 136, 3,   3,   136, 197, 198, 4,   3,   5,   12,  13,  67,  68,  69,
    70,  71,  72,  73,  74,  75,  76,  77,  79,  83,  84,  101, 102, 103, 104, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 194, 194, 4,
    134, 22,  20,  3,   126, 125, 112, 191, 4,   7,   88,  90,  92,  93,  94,  97,  136, 113, 4,
    7,   90,  92,  93,  94,  95,  96,  99,  191, 6,   3,   5,   8,   9,   10,  11,  12,  13,  78,
    80,  81,  82,  101, 102, 103, 104, 106, 110, 111, 113, 114, 115, 116, 118, 119, 120, 135, 138,
    140, 141, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 135, 4,   21,  112, 23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  142, 121, 137, 138, 190, 108,
    5,   5,   139, 5,   5,   5,   5,   5,   139, 139, 139, 139, 139, 139, 139, 197, 137, 137, 137,
    137, 137, 137, 137, 137, 137, 137, 137, 5,   137, 137, 137, 5,   137, 5,   5,   5,   5,   137,
    5,   5,   4,   89,  90,  92,  94,  95,  98,  190, 6,   4,   137, 137, 4,   137, 112, 112, 137,
    137, 4,   4,   4,   4,   4,   4,   4,   62,  4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
    4,   137, 4,   4,   4,   137, 4,   137, 137, 137, 137, 4,   137, 137, 137, 137, 137, 137, 6,
    6,   137, 6,   137, 137, 137, 137, 137, 137, 137, 137, 137, 190, 6,   190, 4,   4,   190, 4,
    63,  6,   6,   6,   190, 6,   6,   6,   6,   6,   6,   6,   105, 107, 108, 109, 113, 162, 4,
    4,   4,   6,   4,   4,   4,   4,   4,   4,   4,   64,  188, 4,   137, 65,  189, 137, 4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   87,  195, 195, 196, 126, 126, 198, 197, 127, 127, 127, 127, 127, 127, 133, 128, 129, 136,
    136, 136, 136, 130, 131, 132, 134, 134, 97,  97,  135, 135, 135, 135, 135, 135, 135, 135, 135,
    135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 135, 88,  88,  88,
    88,  192, 193, 193, 100, 100, 194, 91,  91,  91,  91,  94,  90,  90,  90,  90,  90,  90,  92,
    92,  92,  92,  92,  92,  92,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,  93,
    93,  93,  93,  93,  93,  93,  93,  113, 114, 115, 116, 118, 119, 120, 101, 102, 103, 104, 106,
    110, 111, 105, 105, 107, 107, 108, 108, 109, 109, 117, 117, 121, 121, 121, 121, 121, 121, 121,
    121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 121, 190, 190, 137, 137, 139, 138, 138,
    138, 138, 138, 138, 138, 140, 141, 142, 142, 98,  89,  89,  89,  89,  95,  143, 143, 143, 143,
    143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 143, 144, 145, 173, 174, 175, 176,
    177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 146, 146, 146, 147, 148, 149, 150, 150,
    151, 152, 112, 112, 122, 122, 123, 191, 191, 124, 125, 125, 99,  96,  96,  96,  96,  96,  153,
    153, 153, 153, 153, 153, 153, 154, 155, 156, 157, 158, 159, 160, 161, 161, 161, 161, 161, 161,
    161, 161, 161, 161, 162, 162, 162, 162, 162, 188, 188, 189, 189, 163, 164, 165, 166, 167, 168,
    169, 170, 171, 172};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7, 1,  1, 1, 1, 2, 2, 4, 0, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    3, 0, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  0, 2, 1, 1, 4, 1, 1, 1, 1, 1,
    1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    8, 4, 4, 4, 7, 4, 4, 4, 7, 4, 7, 8, 7, 7, 4, 7, 7, 1, 1,  1, 8, 8, 6, 1, 1, 6, 6, 1, 1,
    1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 4, 4, 4, 4, 4, 4, 4, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "ABS",
                                                   "CEIL",
                                                   "DIVIDE",
                                                   "EXPONENT",
                                                   "FLOOR",
                                                   "LN",
                                                   "LOG",
                                                   "LOGTEN",
                                                   "MOD",
                                                   "MULTIPLY",
                                                   "POW",
                                                   "ROUND",
                                                   "SQRT",
                                                   "SUBTRACT",
                                                   "TRUNC",
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
                                                   "START_PIPELINE",
                                                   "START_MATCH",
                                                   "$accept",
                                                   "projectionFieldname",
                                                   "expressionFieldname",
                                                   "stageAsUserFieldname",
                                                   "filterFieldname",
                                                   "argAsUserFieldname",
                                                   "aggExprAsUserFieldname",
                                                   "invariableUserFieldname",
                                                   "idAsUserFieldname",
                                                   "valueFieldname",
                                                   "projectField",
                                                   "expressionField",
                                                   "valueField",
                                                   "filterField",
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
                                                   "abs",
                                                   "ceil",
                                                   "divide",
                                                   "exponent",
                                                   "floor",
                                                   "ln",
                                                   "log",
                                                   "logten",
                                                   "mod",
                                                   "multiply",
                                                   "pow",
                                                   "round",
                                                   "sqrt",
                                                   "subtract",
                                                   "trunc",
                                                   "onErrorArg",
                                                   "onNullArg",
                                                   "expressions",
                                                   "values",
                                                   "matchExpression",
                                                   "filterFields",
                                                   "filterVal",
                                                   "start",
                                                   "pipeline",
                                                   "START_ORDERED_OBJECT",
                                                   "$@1",
                                                   YY_NULLPTR};
#endif


#if YYDEBUG
const short PipelineParserGen::yyrline_[] = {
    0,    249,  249,  250,  257,  263,  264,  272,  272,  275,  275,  275,  275,  275,  275,  278,
    288,  294,  304,  304,  304,  304,  308,  313,  318,  334,  337,  344,  347,  353,  354,  355,
    356,  357,  358,  359,  360,  361,  362,  363,  364,  367,  370,  373,  376,  379,  382,  385,
    388,  391,  394,  395,  396,  397,  401,  401,  401,  401,  405,  411,  414,  421,  424,  430,
    434,  434,  434,  434,  438,  446,  449,  452,  455,  458,  461,  470,  473,  476,  479,  482,
    485,  488,  496,  499,  502,  505,  508,  511,  514,  517,  520,  523,  526,  529,  532,  535,
    538,  541,  544,  547,  550,  553,  556,  559,  562,  565,  568,  571,  574,  577,  580,  583,
    586,  589,  592,  595,  598,  601,  604,  607,  610,  617,  623,  629,  635,  641,  647,  653,
    659,  665,  671,  677,  683,  689,  695,  701,  704,  710,  713,  719,  722,  728,  731,  737,
    740,  746,  747,  748,  749,  750,  751,  752,  753,  754,  755,  756,  757,  758,  759,  760,
    761,  762,  763,  764,  771,  772,  779,  779,  783,  788,  788,  788,  788,  788,  788,  789,
    795,  803,  809,  812,  819,  826,  826,  826,  826,  830,  836,  836,  836,  836,  836,  836,
    836,  836,  836,  836,  836,  836,  836,  837,  837,  837,  837,  841,  851,  857,  862,  867,
    873,  878,  883,  888,  894,  899,  905,  914,  920,  926,  931,  937,  943,  943,  943,  947,
    957,  967,  974,  974,  978,  985,  992,  992,  996,  996,  1000, 1006, 1007, 1014, 1020, 1023,
    1030, 1037, 1038, 1039, 1040, 1041, 1044, 1044, 1044, 1044, 1044, 1044, 1044, 1046, 1051, 1056,
    1061, 1066, 1071, 1076, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090, 1091, 1097, 1097,
    1097, 1097, 1097, 1101, 1104, 1111, 1114, 1120, 1129, 1134, 1139, 1144, 1149, 1154, 1159, 1164,
    1169};

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
#line 4298 "pipeline_parser_gen.cpp"

#line 1173 "pipeline_grammar.yy"
