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
        case 72:  // BINARY
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 79:  // JAVASCRIPT
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 78:  // DB_POINTER
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 77:  // REGEX
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 80:  // SYMBOL
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 75:  // DATE_LITERAL
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 74:  // OBJECT_ID
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 83:  // TIMESTAMP
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 88:  // MAX_KEY
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 87:  // MIN_KEY
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 76:  // JSNULL
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 73:  // UNDEFINED
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 82:  // INT_NON_ZERO
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 84:  // LONG_NON_ZERO
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 201:  // expressions
        case 202:  // values
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
        case 72:  // BINARY
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 79:  // JAVASCRIPT
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 78:  // DB_POINTER
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 77:  // REGEX
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 80:  // SYMBOL
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 75:  // DATE_LITERAL
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 74:  // OBJECT_ID
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 83:  // TIMESTAMP
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 88:  // MAX_KEY
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 87:  // MIN_KEY
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 76:  // JSNULL
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 73:  // UNDEFINED
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.move<double>(YY_MOVE(that.value));
            break;

        case 82:  // INT_NON_ZERO
            value.move<int>(YY_MOVE(that.value));
            break;

        case 84:  // LONG_NON_ZERO
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 201:  // expressions
        case 202:  // values
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
        case 72:  // BINARY
            value.copy<BSONBinData>(that.value);
            break;

        case 79:  // JAVASCRIPT
            value.copy<BSONCode>(that.value);
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 78:  // DB_POINTER
            value.copy<BSONDBRef>(that.value);
            break;

        case 77:  // REGEX
            value.copy<BSONRegEx>(that.value);
            break;

        case 80:  // SYMBOL
            value.copy<BSONSymbol>(that.value);
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 75:  // DATE_LITERAL
            value.copy<Date_t>(that.value);
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.copy<Decimal128>(that.value);
            break;

        case 74:  // OBJECT_ID
            value.copy<OID>(that.value);
            break;

        case 83:  // TIMESTAMP
            value.copy<Timestamp>(that.value);
            break;

        case 88:  // MAX_KEY
            value.copy<UserMaxKey>(that.value);
            break;

        case 87:  // MIN_KEY
            value.copy<UserMinKey>(that.value);
            break;

        case 76:  // JSNULL
            value.copy<UserNull>(that.value);
            break;

        case 73:  // UNDEFINED
            value.copy<UserUndefined>(that.value);
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.copy<double>(that.value);
            break;

        case 82:  // INT_NON_ZERO
            value.copy<int>(that.value);
            break;

        case 84:  // LONG_NON_ZERO
            value.copy<long long>(that.value);
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.copy<std::string>(that.value);
            break;

        case 201:  // expressions
        case 202:  // values
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
        case 72:  // BINARY
            value.move<BSONBinData>(that.value);
            break;

        case 79:  // JAVASCRIPT
            value.move<BSONCode>(that.value);
            break;

        case 81:  // JAVASCRIPT_W_SCOPE
            value.move<BSONCodeWScope>(that.value);
            break;

        case 78:  // DB_POINTER
            value.move<BSONDBRef>(that.value);
            break;

        case 77:  // REGEX
            value.move<BSONRegEx>(that.value);
            break;

        case 80:  // SYMBOL
            value.move<BSONSymbol>(that.value);
            break;

        case 107:  // dbPointer
        case 108:  // javascript
        case 109:  // symbol
        case 110:  // javascriptWScope
        case 111:  // int
        case 112:  // timestamp
        case 113:  // long
        case 114:  // double
        case 115:  // decimal
        case 116:  // minKey
        case 117:  // maxKey
        case 118:  // value
        case 119:  // string
        case 120:  // binary
        case 121:  // undefined
        case 122:  // objectId
        case 123:  // bool
        case 124:  // date
        case 125:  // null
        case 126:  // regex
        case 127:  // simpleValue
        case 128:  // compoundValue
        case 129:  // valueArray
        case 130:  // valueObject
        case 131:  // valueFields
        case 132:  // dollarString
        case 133:  // nonDollarString
        case 134:  // stageList
        case 135:  // stage
        case 136:  // inhibitOptimization
        case 137:  // unionWith
        case 138:  // skip
        case 139:  // limit
        case 140:  // project
        case 141:  // sample
        case 142:  // unwind
        case 143:  // projectFields
        case 144:  // projection
        case 145:  // num
        case 148:  // expression
        case 149:  // compoundExpression
        case 150:  // exprFixedTwoArg
        case 151:  // expressionArray
        case 152:  // expressionObject
        case 153:  // expressionFields
        case 154:  // maths
        case 155:  // add
        case 156:  // atan2
        case 157:  // boolExps
        case 158:  // and
        case 159:  // or
        case 160:  // not
        case 161:  // literalEscapes
        case 162:  // const
        case 163:  // literal
        case 164:  // compExprs
        case 165:  // cmp
        case 166:  // eq
        case 167:  // gt
        case 168:  // gte
        case 169:  // lt
        case 170:  // lte
        case 171:  // ne
        case 172:  // typeExpression
        case 173:  // typeValue
        case 174:  // convert
        case 175:  // toBool
        case 176:  // toDate
        case 177:  // toDecimal
        case 178:  // toDouble
        case 179:  // toInt
        case 180:  // toLong
        case 181:  // toObjectId
        case 182:  // toString
        case 183:  // type
        case 184:  // abs
        case 185:  // ceil
        case 186:  // divide
        case 187:  // exponent
        case 188:  // floor
        case 189:  // ln
        case 190:  // log
        case 191:  // logten
        case 192:  // mod
        case 193:  // multiply
        case 194:  // pow
        case 195:  // round
        case 196:  // sqrt
        case 197:  // subtract
        case 198:  // trunc
        case 203:  // matchExpression
        case 204:  // filterFields
        case 205:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 94:   // projectionFieldname
        case 95:   // expressionFieldname
        case 96:   // stageAsUserFieldname
        case 97:   // filterFieldname
        case 98:   // argAsUserFieldname
        case 99:   // aggExprAsUserFieldname
        case 100:  // invariableUserFieldname
        case 101:  // idAsUserFieldname
        case 102:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 75:  // DATE_LITERAL
            value.move<Date_t>(that.value);
            break;

        case 86:  // DECIMAL_NON_ZERO
            value.move<Decimal128>(that.value);
            break;

        case 74:  // OBJECT_ID
            value.move<OID>(that.value);
            break;

        case 83:  // TIMESTAMP
            value.move<Timestamp>(that.value);
            break;

        case 88:  // MAX_KEY
            value.move<UserMaxKey>(that.value);
            break;

        case 87:  // MIN_KEY
            value.move<UserMinKey>(that.value);
            break;

        case 76:  // JSNULL
            value.move<UserNull>(that.value);
            break;

        case 73:  // UNDEFINED
            value.move<UserUndefined>(that.value);
            break;

        case 85:  // DOUBLE_NON_ZERO
            value.move<double>(that.value);
            break;

        case 82:  // INT_NON_ZERO
            value.move<int>(that.value);
            break;

        case 84:  // LONG_NON_ZERO
            value.move<long long>(that.value);
            break;

        case 103:  // projectField
        case 104:  // expressionField
        case 105:  // valueField
        case 106:  // filterField
        case 146:  // includeArrayIndexArg
        case 147:  // preserveNullAndEmptyArraysArg
        case 199:  // onErrorArg
        case 200:  // onNullArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 70:  // FIELDNAME
        case 71:  // NONEMPTY_STRING
        case 91:  // "a $-prefixed string"
        case 92:  // "an empty string"
            value.move<std::string>(that.value);
            break;

        case 201:  // expressions
        case 202:  // values
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
                case 72:  // BINARY
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 79:  // JAVASCRIPT
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 81:  // JAVASCRIPT_W_SCOPE
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 78:  // DB_POINTER
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 77:  // REGEX
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 80:  // SYMBOL
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 107:  // dbPointer
                case 108:  // javascript
                case 109:  // symbol
                case 110:  // javascriptWScope
                case 111:  // int
                case 112:  // timestamp
                case 113:  // long
                case 114:  // double
                case 115:  // decimal
                case 116:  // minKey
                case 117:  // maxKey
                case 118:  // value
                case 119:  // string
                case 120:  // binary
                case 121:  // undefined
                case 122:  // objectId
                case 123:  // bool
                case 124:  // date
                case 125:  // null
                case 126:  // regex
                case 127:  // simpleValue
                case 128:  // compoundValue
                case 129:  // valueArray
                case 130:  // valueObject
                case 131:  // valueFields
                case 132:  // dollarString
                case 133:  // nonDollarString
                case 134:  // stageList
                case 135:  // stage
                case 136:  // inhibitOptimization
                case 137:  // unionWith
                case 138:  // skip
                case 139:  // limit
                case 140:  // project
                case 141:  // sample
                case 142:  // unwind
                case 143:  // projectFields
                case 144:  // projection
                case 145:  // num
                case 148:  // expression
                case 149:  // compoundExpression
                case 150:  // exprFixedTwoArg
                case 151:  // expressionArray
                case 152:  // expressionObject
                case 153:  // expressionFields
                case 154:  // maths
                case 155:  // add
                case 156:  // atan2
                case 157:  // boolExps
                case 158:  // and
                case 159:  // or
                case 160:  // not
                case 161:  // literalEscapes
                case 162:  // const
                case 163:  // literal
                case 164:  // compExprs
                case 165:  // cmp
                case 166:  // eq
                case 167:  // gt
                case 168:  // gte
                case 169:  // lt
                case 170:  // lte
                case 171:  // ne
                case 172:  // typeExpression
                case 173:  // typeValue
                case 174:  // convert
                case 175:  // toBool
                case 176:  // toDate
                case 177:  // toDecimal
                case 178:  // toDouble
                case 179:  // toInt
                case 180:  // toLong
                case 181:  // toObjectId
                case 182:  // toString
                case 183:  // type
                case 184:  // abs
                case 185:  // ceil
                case 186:  // divide
                case 187:  // exponent
                case 188:  // floor
                case 189:  // ln
                case 190:  // log
                case 191:  // logten
                case 192:  // mod
                case 193:  // multiply
                case 194:  // pow
                case 195:  // round
                case 196:  // sqrt
                case 197:  // subtract
                case 198:  // trunc
                case 203:  // matchExpression
                case 204:  // filterFields
                case 205:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 94:   // projectionFieldname
                case 95:   // expressionFieldname
                case 96:   // stageAsUserFieldname
                case 97:   // filterFieldname
                case 98:   // argAsUserFieldname
                case 99:   // aggExprAsUserFieldname
                case 100:  // invariableUserFieldname
                case 101:  // idAsUserFieldname
                case 102:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 75:  // DATE_LITERAL
                    yylhs.value.emplace<Date_t>();
                    break;

                case 86:  // DECIMAL_NON_ZERO
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 74:  // OBJECT_ID
                    yylhs.value.emplace<OID>();
                    break;

                case 83:  // TIMESTAMP
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 88:  // MAX_KEY
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 87:  // MIN_KEY
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 76:  // JSNULL
                    yylhs.value.emplace<UserNull>();
                    break;

                case 73:  // UNDEFINED
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 85:  // DOUBLE_NON_ZERO
                    yylhs.value.emplace<double>();
                    break;

                case 82:  // INT_NON_ZERO
                    yylhs.value.emplace<int>();
                    break;

                case 84:  // LONG_NON_ZERO
                    yylhs.value.emplace<long long>();
                    break;

                case 103:  // projectField
                case 104:  // expressionField
                case 105:  // valueField
                case 106:  // filterField
                case 146:  // includeArrayIndexArg
                case 147:  // preserveNullAndEmptyArraysArg
                case 199:  // onErrorArg
                case 200:  // onNullArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 70:  // FIELDNAME
                case 71:  // NONEMPTY_STRING
                case 91:  // "a $-prefixed string"
                case 92:  // "an empty string"
                    yylhs.value.emplace<std::string>();
                    break;

                case 201:  // expressions
                case 202:  // values
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
#line 261 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1557 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 268 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1565 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 274 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 1571 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 275 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1579 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 283 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1585 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1591 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1597 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1603 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1609 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1615 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1621 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 286 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1627 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 290 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unwind, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1635 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 294 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unwind,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::pathArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                YY_MOVE(yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                YY_MOVE(yystack_[1]
                                            .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 1649 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 307 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::includeArrayIndexArg, CNode{KeyValue::absentKey}};
                    }
#line 1657 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 310 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::includeArrayIndexArg,
                                      YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1665 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 317 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::preserveNullAndEmptyArraysArg,
                                      CNode{KeyValue::absentKey}};
                    }
#line 1673 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 320 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::preserveNullAndEmptyArraysArg,
                                      YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1681 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 325 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1693 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 335 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1701 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 341 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1714 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1720 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1726 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1732 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 351 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1738 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 355 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1746 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 360 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1754 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 365 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::project, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1762 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 371 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1770 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 374 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1779 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 381 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1787 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 384 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1795 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 390 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1801 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 391 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1809 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 394 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1817 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 397 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1825 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 400 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1833 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 403 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1841 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 406 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1849 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 409 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1857 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 412 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1865 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 415 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 1873 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 418 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 1881 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 421 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1887 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1893 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1899 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1905 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 425 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1911 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 429 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 1919 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 435 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1927 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 438 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1936 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 445 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1944 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 448 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1952 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 454 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1958 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1964 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1970 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1976 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 458 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 1982 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 462 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 1990 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 470 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 1998 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 473 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2006 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 476 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2014 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 479 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2022 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 482 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2030 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 485 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2038 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 488 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unwind"};
                    }
#line 2046 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 497 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2054 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 500 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2062 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 503 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2070 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 506 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2078 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 509 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2086 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 512 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2094 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 515 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2102 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 518 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"path"};
                    }
#line 2110 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 521 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"includeArrayIndex"};
                    }
#line 2118 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 524 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"preserveNullAndEmptyArrays"};
                    }
#line 2126 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 532 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2134 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 535 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2142 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 538 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2150 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 541 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2158 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 544 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2166 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 547 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2174 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 550 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2182 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 553 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2190 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 556 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2198 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 559 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2206 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 562 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2214 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 565 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2222 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 568 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2230 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 571 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2238 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 574 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2246 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 577 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2254 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 580 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2262 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 583 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2270 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 586 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2278 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 589 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2286 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 592 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2294 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 595 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2302 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 598 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2310 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 601 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2318 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 604 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2326 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 607 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2334 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 610 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2342 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 613 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2350 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 616 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2358 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 619 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2366 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 622 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2374 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 625 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2382 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 628 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2390 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 631 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2398 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 634 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2406 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 637 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2414 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 640 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 643 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2430 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 646 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2438 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 653 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2446 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 656 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2454 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 659 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2462 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 666 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2470 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 673 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2478 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 679 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2486 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 685 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2494 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 691 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2502 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 697 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2510 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 703 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2518 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 709 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2526 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 715 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2534 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 721 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2542 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 727 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2550 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 733 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2558 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 739 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2566 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 745 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2574 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 751 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2582 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 757 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2590 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 760 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2598 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 766 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2606 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 769 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2614 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 775 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2622 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 778 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2630 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 784 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2638 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 787 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 2646 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 793 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 2654 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 796 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 2662 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 802 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2668 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 803 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2674 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 804 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2680 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 805 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2686 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 806 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2692 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 807 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2698 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 808 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2704 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 809 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2710 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 810 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2716 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 811 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2722 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 812 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2728 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 813 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2734 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 814 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2740 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 815 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2746 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 816 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2752 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 817 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2758 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 818 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2764 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 819 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2770 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 820 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2776 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 827 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 2782 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 828 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 2791 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 835 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2797 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 835 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2803 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 839 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 2811 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2817 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2823 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2829 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2835 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2841 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 844 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2847 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 845 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2853 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 851 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2861 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 859 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2869 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 865 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2877 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 868 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2886 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 875 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2894 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 882 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2900 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 882 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2906 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 882 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2912 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 882 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2918 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 886 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 2926 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2932 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2938 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2944 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2950 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2956 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2962 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2968 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2974 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2980 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2986 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2992 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2998 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 892 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3004 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 893 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3010 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 893 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3016 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 893 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3022 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 893 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3028 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 897 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3040 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 907 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3049 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 913 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3057 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 918 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3065 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 923 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3074 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 929 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3082 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 934 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3090 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 939 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3098 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 944 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3107 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 950 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3115 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 955 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3124 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 961 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3136 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 970 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3145 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 976 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3154 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 982 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3162 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 987 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3171 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 993 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3180 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 999 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3186 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 999 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3192 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 999 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3198 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 1003 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3210 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 1013 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3222 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 1023 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3231 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 1030 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3237 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 1030 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3243 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 1034 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3252 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 1041 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3261 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 1048 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3267 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 1048 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3273 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 1052 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3279 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 1052 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3285 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 1056 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3293 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 1062 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                    }
#line 3299 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 1063 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3308 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 1070 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3316 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 1076 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3324 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1079 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3333 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1086 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3341 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1093 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3347 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1094 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3353 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1095 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3359 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1096 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3365 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1097 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3371 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3377 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3383 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3389 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3395 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3401 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3407 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1100 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3413 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1102 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3422 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1107 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3431 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1112 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3440 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1117 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3449 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1122 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3458 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1127 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3467 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1132 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3476 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1138 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3482 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1139 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3488 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1140 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3494 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1141 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3500 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1142 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3506 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1143 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3512 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1144 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3518 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1145 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3524 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1146 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3530 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1147 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3536 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1153 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3542 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1153 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3548 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1153 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3554 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1153 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3560 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 276:
#line 1153 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3566 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 277:
#line 1157 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 3574 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 278:
#line 1160 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3582 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 279:
#line 1167 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 3590 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 280:
#line 1170 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3598 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 281:
#line 1176 "src/mongo/db/cst/pipeline_grammar.yy"
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
#line 3609 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 282:
#line 1185 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3617 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 283:
#line 1190 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3625 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 284:
#line 1195 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3633 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 285:
#line 1200 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3641 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 286:
#line 1205 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3649 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 287:
#line 1210 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3657 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 288:
#line 1215 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3665 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 289:
#line 1220 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3673 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;

                    case 290:
#line 1225 "src/mongo/db/cst/pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3681 "src/mongo/db/cst/pipeline_parser_gen.cpp"
                    break;


#line 3685 "src/mongo/db/cst/pipeline_parser_gen.cpp"

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


const short PipelineParserGen::yypact_ninf_ = -289;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -44,  8,    12,   41,   48,   -289, -289, -289, -289, 53,   59,   413,  51,   39,   71,   72,
    39,   -289, -15,  73,   -289, -289, -289, -289, -289, -289, -289, -289, -289, 174,  -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, 174,  -289, -289, -289, -289, 74,   -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, 57,   -289, 60,   79,
    -289, -289, 65,   48,   -289, 174,  -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, 480,  39,   -31,  -289, -15,  -289, 547,  174,  86,   -289, 265,  265,  -289,
    -289, -289, -289, -289, 90,   81,   84,   -289, -289, -289, -289, -289, -289, -289, 174,  -289,
    -289, -289, 591,  223,  -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, 23,   35,   85,   -289, 107,  108,  109,  111,  113,  117,  122,  108,  108,  108,  108,
    108,  108,  108,  -289, 223,  223,  223,  223,  223,  223,  223,  223,  223,  223,  223,  123,
    223,  223,  223,  124,  223,  125,  127,  128,  129,  223,  131,  136,  309,  -289, 223,  -289,
    138,  141,  -289, -289, 44,   142,  223,  223,  143,  223,  174,  174,  223,  223,  145,  146,
    150,  151,  152,  154,  155,  95,   158,  159,  160,  161,  163,  165,  166,  167,  168,  171,
    172,  223,  176,  177,  184,  223,  185,  223,  223,  223,  223,  186,  223,  223,  -289, 223,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, 223,  223,  -289, 223,  187,  188,
    223,  189,  -289, -289, -289, -289, -289, -289, -289, 223,  -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, 223,  -289, -289, -289, 223,  -289, 223,  223,  223,  223,  -289,
    223,  223,  -289, 223,  190,  223,  194,  195,  223,  196,  135,  197,  198,  199,  223,  200,
    201,  204,  205,  206,  -289, 207,  -289, -289, 208,  -289, 277,  211,  212,  213,  214,  215,
    217,  218,  219,  220,  221,  225,  -289, -289, -289, -289, -289, 110,  -289, -289, -289, 226,
    -289, -289, -289, -289, -289, -289, -289, 223,  149,  -289, -289, 223,  233,  -289, 234,  -289};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   53,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   7,
    0,   9,   10,  11,  12,  13,  14,  15,  4,   52,  0,   63,  66,  67,  68,  65,  64,  69,  70,
    71,  72,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,
    94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 116, 117, 118, 73,  74,  75,  76,  62,  59,  0,   60,  61,  58,  54,  0,   138,
    140, 142, 144, 137, 139, 141, 143, 25,  26,  27,  28,  30,  32,  0,   29,  0,   0,   122, 16,
    0,   5,   240, 237, 145, 146, 119, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 120, 121, 154, 155, 156, 157, 158, 163, 159, 160, 161, 164, 165, 57,  147, 148, 149, 150,
    162, 151, 152, 153, 232, 233, 234, 235, 55,  56,  23,  0,   0,   0,   8,   0,   6,   0,   237,
    0,   31,  0,   0,   49,  50,  51,  48,  33,  0,   0,   18,  239, 187, 244, 245, 246, 243, 247,
    0,   241, 238, 236, 180, 166, 38,  40,  42,  44,  45,  46,  37,  39,  41,  43,  36,  34,  47,
    171, 172, 173, 188, 189, 174, 222, 223, 224, 175, 228, 229, 176, 248, 249, 250, 251, 252, 253,
    254, 177, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 190, 191, 192, 193, 194, 195, 196,
    197, 198, 199, 200, 201, 202, 203, 204, 35,  22,  0,   0,   20,  242, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   168, 166, 169,
    0,   0,   123, 19,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   179, 0,   184, 185, 183, 186, 181, 167, 178, 24,  21,
    17,  0,   0,   206, 0,   0,   0,   0,   0,   255, 256, 257, 258, 259, 260, 261, 0,   282, 283,
    284, 285, 286, 287, 288, 289, 290, 207, 208, 0,   210, 211, 212, 0,   214, 0,   0,   0,   0,
    219, 0,   0,   182, 166, 0,   166, 0,   0,   166, 0,   0,   0,   0,   0,   166, 0,   0,   0,
    0,   0,   170, 0,   230, 231, 0,   227, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   273, 274, 275, 276, 272, 277, 209, 213, 215, 0,   217, 218, 220, 221, 205, 225, 226, 0,
    279, 216, 278, 0,   0,   280, 0,   281};

const short PipelineParserGen::yypgoto_[] = {
    -289, -289, -289, -134, -289, -127, -125, -126, -109, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -12,  -289, -11,  -13,  -7,   -289, -289, -106, -157, -289, -289, -289,
    -116, -289, -289, -289, -22,  -289, -289, -289, -289, 75,   -289, 112,  -289, -289, -289,
    -289, -289, -289, -289, -289, -289, 66,   -4,   -289, -289, -258, -111, -172, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289, -289,
    -289, -289, -289, -288, 69,   -289, -289, 153,  -289, -289, -8,   -289};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  174, 351, 87,  88,  89,  90,  91,  189, 190, 179, 356, 191, 92,  136, 137, 138,
    139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
    301, 157, 158, 159, 169, 113, 307, 10,  19,  20,  21,  22,  23,  24,  25,  26,  163,
    207, 106, 259, 309, 302, 303, 312, 209, 210, 300, 211, 212, 213, 214, 215, 216, 217,
    218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 443, 230, 231, 232, 233,
    234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250,
    251, 252, 253, 254, 456, 460, 304, 171, 7,   11,  160, 3,   5,   110, 111};

const short PipelineParserGen::yytable_[] = {
    104, 102, 103, 104, 102, 103, 105, 156, 181, 105, 114, 170, 109, 4,   357, 6,   206, 206, 326,
    327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 175, 338, 339, 340, 96,  342, 185, 176, 178,
    177, 347, 120, 8,   186, 188, 187, 1,   2,   94,  95,  96,  97,  9,   362, 363, 93,  365, 118,
    119, 368, 369, 134, 135, 208, 208, 170, 27,  156, 12,  13,  14,  15,  16,  17,  18,  107, 108,
    112, 115, 162, 389, 164, 165, 166, 393, 260, 395, 396, 397, 398, 167, 400, 401, 193, 402, 256,
    156, 318, 319, 320, 321, 322, 323, 324, 257, 403, 404, 306, 405, 100, 258, 408, 308, 310, 311,
    313, 419, 314, 421, 315, 410, 424, 98,  316, 99,  100, 101, 430, 317, 337, 341, 343, 411, 344,
    345, 346, 412, 348, 413, 414, 415, 416, 349, 417, 418, 358, 359, 361, 364, 156, 370, 371, 104,
    102, 103, 372, 373, 374, 105, 375, 376, 180, 377, 378, 379, 380, 381, 352, 382, 156, 383, 384,
    385, 386, 353, 354, 387, 388, 116, 455, 117, 390, 391, 94,  95,  96,  97,  118, 119, 392, 394,
    399, 355, 360, 406, 407, 409, 420, 458, 422, 423, 425, 461, 426, 427, 428, 429, 431, 432, 366,
    367, 433, 434, 435, 436, 437, 444, 445, 446, 459, 448, 447, 449, 450, 451, 452, 453, 194, 168,
    195, 454, 457, 94,  95,  96,  97,  118, 119, 462, 463, 192, 255, 161, 182, 0,   305, 120, 121,
    122, 123, 124, 125, 126, 127, 128, 129, 130, 98,  131, 99,  100, 101, 132, 133, 0,   0,   134,
    135, 325, 194, 442, 195, 0,   0,   196, 197, 198, 199, 200, 201, 0,   0,   0,   0,   0,   0,
    94,  95,  96,  97,  0,   0,   0,   156, 156, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
    130, 98,  131, 99,  100, 101, 132, 133, 0,   350, 134, 135, 184, 0,   0,   0,   0,   0,   0,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  120, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   202, 120, 203, 204, 205, 0,   0,   0,   0,   134, 135, 0,   98,  0,
    99,  100, 101, 0,   0,   0,   0,   134, 135, 0,   0,   0,   0,   0,   82,  83,  84,  85,  86,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   440, 438, 439, 0,   28,
    0,   441, 29,  0,   0,   0,   0,   0,   0,   30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  172, 0,   0,   173, 0,   0,   0,   0,   0,   0,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,
    68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,
    183, 0,   0,   184, 0,   0,   0,   0,   0,   0,   30,  31,  32,  33,  34,  35,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  261, 262, 263, 264, 265, 266, 267, 268, 269,
    270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288,
    289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299};

const short PipelineParserGen::yycheck_[] = {
    13,  13,  13,  16,  16,  16,  13,  29,  165, 16,  18,  117, 16,  5,   302, 3,   173, 174, 276,
    277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 163, 288, 289, 290, 10,  292, 169, 163, 163,
    163, 297, 71,  0,   169, 169, 169, 89,  90,  8,   9,   10,  11,  3,   310, 311, 3,   313, 12,
    13,  316, 317, 91,  92,  173, 174, 170, 6,   88,  14,  15,  16,  17,  18,  19,  20,  3,   3,
    91,  4,   4,   337, 23,  21,  3,   341, 190, 343, 344, 345, 346, 24,  348, 349, 6,   351, 4,
    117, 268, 269, 270, 271, 272, 273, 274, 22,  362, 363, 71,  365, 85,  25,  368, 26,  5,   5,
    5,   403, 5,   405, 5,   377, 408, 82,  5,   84,  85,  86,  414, 5,   5,   5,   5,   389, 5,
    5,   5,   393, 5,   395, 396, 397, 398, 5,   400, 401, 6,   4,   4,   4,   170, 4,   4,   164,
    164, 164, 4,   4,   4,   164, 4,   4,   164, 66,  4,   4,   4,   4,   300, 4,   190, 4,   4,
    4,   4,   300, 300, 4,   4,   3,   68,  5,   4,   4,   8,   9,   10,  11,  12,  13,  4,   4,
    4,   300, 308, 6,   6,   6,   6,   455, 4,   4,   4,   459, 67,  6,   6,   6,   6,   6,   314,
    315, 6,   6,   6,   6,   6,   4,   4,   4,   69,  4,   6,   4,   4,   4,   4,   4,   3,   115,
    5,   4,   4,   8,   9,   10,  11,  12,  13,  4,   4,   170, 174, 88,  167, -1,  257, 71,  72,
    73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  -1,  -1,  91,
    92,  275, 3,   426, 5,   -1,  -1,  8,   9,   10,  11,  12,  13,  -1,  -1,  -1,  -1,  -1,  -1,
    8,   9,   10,  11,  -1,  -1,  -1,  314, 315, 71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  -1,  4,   91,  92,  7,   -1,  -1,  -1,  -1,  -1,  -1,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  71,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  82,  71,  84,  85,  86,  -1,  -1,  -1,  -1,  91,  92,  -1,  82,  -1,
    84,  85,  86,  -1,  -1,  -1,  -1,  91,  92,  -1,  -1,  -1,  -1,  -1,  66,  67,  68,  69,  70,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  426, 426, 426, -1,  4,
    -1,  426, 7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
    62,  63,  64,  65,  66,  67,  68,  69,  70,  4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    4,   -1,  -1,  7,   -1,  -1,  -1,  -1,  -1,  -1,  14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,
    61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  27,  28,  29,  30,  31,  32,  33,  34,  35,
    36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65};

const unsigned char PipelineParserGen::yystos_[] = {
    0,   89,  90,  206, 5,   207, 3,   203, 0,   3,   134, 204, 14,  15,  16,  17,  18,  19,  20,
    135, 136, 137, 138, 139, 140, 141, 142, 6,   4,   7,   14,  15,  16,  17,  18,  19,  20,  21,
    22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  96,  97,  98,  99,  100, 106, 3,   8,
    9,   10,  11,  82,  84,  85,  86,  111, 113, 114, 115, 145, 3,   3,   145, 208, 209, 91,  132,
    208, 4,   3,   5,   12,  13,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  83,  87,
    88,  91,  92,  107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
    123, 124, 125, 126, 127, 128, 129, 130, 205, 205, 4,   143, 23,  21,  3,   24,  134, 131, 118,
    202, 4,   7,   94,  96,  98,  99,  100, 103, 145, 119, 132, 4,   7,   96,  98,  99,  100, 101,
    102, 105, 202, 6,   3,   5,   8,   9,   10,  11,  12,  13,  82,  84,  85,  86,  119, 144, 149,
    151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170,
    171, 172, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 195, 196, 197, 198, 144, 4,   22,  25,  146, 118, 27,  28,  29,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,
    51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  153, 127, 148, 149,
    201, 114, 71,  133, 26,  147, 5,   5,   150, 5,   5,   5,   5,   5,   150, 150, 150, 150, 150,
    150, 150, 208, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 5,   148, 148, 148, 5,
    148, 5,   5,   5,   5,   148, 5,   5,   4,   95,  96,  98,  100, 101, 104, 201, 6,   4,   123,
    4,   148, 148, 4,   148, 118, 118, 148, 148, 4,   4,   4,   4,   4,   4,   4,   66,  4,   4,
    4,   4,   4,   4,   4,   4,   4,   4,   4,   148, 4,   4,   4,   148, 4,   148, 148, 148, 148,
    4,   148, 148, 148, 148, 148, 148, 6,   6,   148, 6,   148, 148, 148, 148, 148, 148, 148, 148,
    148, 201, 6,   201, 4,   4,   201, 4,   67,  6,   6,   6,   201, 6,   6,   6,   6,   6,   6,
    6,   111, 113, 114, 115, 119, 173, 4,   4,   4,   6,   4,   4,   4,   4,   4,   4,   4,   68,
    199, 4,   148, 69,  200, 148, 4,   4};

const unsigned char PipelineParserGen::yyr1_[] = {
    0,   93,  206, 206, 207, 134, 134, 209, 208, 135, 135, 135, 135, 135, 135, 135, 142, 142, 146,
    146, 147, 147, 141, 136, 137, 145, 145, 145, 145, 138, 139, 140, 143, 143, 103, 103, 144, 144,
    144, 144, 144, 144, 144, 144, 144, 144, 144, 144, 94,  94,  94,  94,  203, 204, 204, 106, 106,
    205, 97,  97,  97,  97,  100, 96,  96,  96,  96,  96,  96,  96,  98,  98,  98,  98,  98,  98,
    98,  98,  98,  98,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  119, 119, 119, 132, 133, 120, 121, 122, 124, 125, 126, 107, 108, 109,
    110, 112, 116, 117, 111, 111, 113, 113, 114, 114, 115, 115, 123, 123, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 201, 201, 148, 148, 150,
    149, 149, 149, 149, 149, 149, 149, 151, 152, 153, 153, 104, 95,  95,  95,  95,  101, 154, 154,
    154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 154, 155, 156, 184, 185,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 157, 157, 157, 158, 159, 160,
    161, 161, 162, 163, 118, 118, 128, 128, 129, 202, 202, 130, 131, 131, 105, 102, 102, 102, 102,
    102, 164, 164, 164, 164, 164, 164, 164, 165, 166, 167, 168, 169, 170, 171, 172, 172, 172, 172,
    172, 172, 172, 172, 172, 172, 173, 173, 173, 173, 173, 199, 199, 200, 200, 174, 175, 176, 177,
    178, 179, 180, 181, 182, 183};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1,  1, 1, 1, 1, 2, 7, 0, 2, 0, 2, 5, 3, 7, 1, 1, 1, 1, 2,
    2, 4, 0, 2, 2, 2, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 0, 2, 2, 2, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 0, 2, 1, 1, 4, 1, 1, 1, 1, 1, 1, 1, 3, 3,
    0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 8, 4, 4, 4, 7,
    4, 4, 4, 7, 4, 7, 8, 7, 7, 4, 7, 7,  1, 1, 1, 8, 8, 6, 1, 1, 6, 6, 1, 1, 1, 1, 3, 0, 2, 3,
    0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
                                                   "STAGE_UNWIND",
                                                   "COLL_ARG",
                                                   "PIPELINE_ARG",
                                                   "SIZE_ARG",
                                                   "PATH_ARG",
                                                   "INCLUDE_ARRAY_INDEX_ARG",
                                                   "PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG",
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
                                                   "NONEMPTY_STRING",
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
                                                   "\"a $-prefixed string\"",
                                                   "\"an empty string\"",
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
                                                   "dollarString",
                                                   "nonDollarString",
                                                   "stageList",
                                                   "stage",
                                                   "inhibitOptimization",
                                                   "unionWith",
                                                   "skip",
                                                   "limit",
                                                   "project",
                                                   "sample",
                                                   "unwind",
                                                   "projectFields",
                                                   "projection",
                                                   "num",
                                                   "includeArrayIndexArg",
                                                   "preserveNullAndEmptyArraysArg",
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
    0,    260,  260,  261,  268,  274,  275,  283,  283,  286,  286,  286,  286,  286,  286,  286,
    290,  293,  307,  310,  317,  320,  325,  335,  341,  351,  351,  351,  351,  355,  360,  365,
    371,  374,  381,  384,  390,  391,  394,  397,  400,  403,  406,  409,  412,  415,  418,  421,
    425,  425,  425,  425,  429,  435,  438,  445,  448,  454,  458,  458,  458,  458,  462,  470,
    473,  476,  479,  482,  485,  488,  497,  500,  503,  506,  509,  512,  515,  518,  521,  524,
    532,  535,  538,  541,  544,  547,  550,  553,  556,  559,  562,  565,  568,  571,  574,  577,
    580,  583,  586,  589,  592,  595,  598,  601,  604,  607,  610,  613,  616,  619,  622,  625,
    628,  631,  634,  637,  640,  643,  646,  653,  656,  659,  666,  673,  679,  685,  691,  697,
    703,  709,  715,  721,  727,  733,  739,  745,  751,  757,  760,  766,  769,  775,  778,  784,
    787,  793,  796,  802,  803,  804,  805,  806,  807,  808,  809,  810,  811,  812,  813,  814,
    815,  816,  817,  818,  819,  820,  827,  828,  835,  835,  839,  844,  844,  844,  844,  844,
    844,  845,  851,  859,  865,  868,  875,  882,  882,  882,  882,  886,  892,  892,  892,  892,
    892,  892,  892,  892,  892,  892,  892,  892,  892,  893,  893,  893,  893,  897,  907,  913,
    918,  923,  929,  934,  939,  944,  950,  955,  961,  970,  976,  982,  987,  993,  999,  999,
    999,  1003, 1013, 1023, 1030, 1030, 1034, 1041, 1048, 1048, 1052, 1052, 1056, 1062, 1063, 1070,
    1076, 1079, 1086, 1093, 1094, 1095, 1096, 1097, 1100, 1100, 1100, 1100, 1100, 1100, 1100, 1102,
    1107, 1112, 1117, 1122, 1127, 1132, 1138, 1139, 1140, 1141, 1142, 1143, 1144, 1145, 1146, 1147,
    1153, 1153, 1153, 1153, 1153, 1157, 1160, 1167, 1170, 1176, 1185, 1190, 1195, 1200, 1205, 1210,
    1215, 1220, 1225};

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
#line 4387 "src/mongo/db/cst/pipeline_parser_gen.cpp"

#line 1229 "src/mongo/db/cst/pipeline_grammar.yy"
