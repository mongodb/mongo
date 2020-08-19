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
#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
}
}  // namespace mongo

// Default location for actions, called each time a rule is matched but before the action is
// run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
// mongo.
#define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)

#line 68 "pipeline_parser_gen.cpp"


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
#line 161 "pipeline_parser_gen.cpp"

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
        case 99:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 106:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 108:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 105:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 104:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 107:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 134:  // dbPointer
        case 135:  // javascript
        case 136:  // symbol
        case 137:  // javascriptWScope
        case 138:  // int
        case 139:  // timestamp
        case 140:  // long
        case 141:  // double
        case 142:  // decimal
        case 143:  // minKey
        case 144:  // maxKey
        case 145:  // value
        case 146:  // string
        case 147:  // fieldPath
        case 148:  // binary
        case 149:  // undefined
        case 150:  // objectId
        case 151:  // bool
        case 152:  // date
        case 153:  // null
        case 154:  // regex
        case 155:  // simpleValue
        case 156:  // compoundValue
        case 157:  // valueArray
        case 158:  // valueObject
        case 159:  // valueFields
        case 160:  // variable
        case 161:  // stageList
        case 162:  // stage
        case 163:  // inhibitOptimization
        case 164:  // unionWith
        case 165:  // skip
        case 166:  // limit
        case 167:  // project
        case 168:  // sample
        case 169:  // projectFields
        case 170:  // projection
        case 171:  // num
        case 172:  // expression
        case 173:  // compoundExpression
        case 174:  // exprFixedTwoArg
        case 175:  // expressionArray
        case 176:  // expressionObject
        case 177:  // expressionFields
        case 178:  // maths
        case 179:  // add
        case 180:  // atan2
        case 181:  // boolExps
        case 182:  // and
        case 183:  // or
        case 184:  // not
        case 185:  // literalEscapes
        case 186:  // const
        case 187:  // literal
        case 188:  // stringExps
        case 189:  // concat
        case 190:  // dateFromString
        case 191:  // dateToString
        case 192:  // indexOfBytes
        case 193:  // indexOfCP
        case 194:  // ltrim
        case 195:  // regexFind
        case 196:  // regexFindAll
        case 197:  // regexMatch
        case 198:  // regexArgs
        case 199:  // replaceOne
        case 200:  // replaceAll
        case 201:  // rtrim
        case 202:  // split
        case 203:  // strLenBytes
        case 204:  // strLenCP
        case 205:  // strcasecmp
        case 206:  // substr
        case 207:  // substrBytes
        case 208:  // substrCP
        case 209:  // toLower
        case 210:  // toUpper
        case 211:  // trim
        case 212:  // compExprs
        case 213:  // cmp
        case 214:  // eq
        case 215:  // gt
        case 216:  // gte
        case 217:  // lt
        case 218:  // lte
        case 219:  // ne
        case 220:  // typeExpression
        case 221:  // convert
        case 222:  // toBool
        case 223:  // toDate
        case 224:  // toDecimal
        case 225:  // toDouble
        case 226:  // toInt
        case 227:  // toLong
        case 228:  // toObjectId
        case 229:  // toString
        case 230:  // type
        case 231:  // abs
        case 232:  // ceil
        case 233:  // divide
        case 234:  // exponent
        case 235:  // floor
        case 236:  // ln
        case 237:  // log
        case 238:  // logten
        case 239:  // mod
        case 240:  // multiply
        case 241:  // pow
        case 242:  // round
        case 243:  // sqrt
        case 244:  // subtract
        case 245:  // trunc
        case 255:  // matchExpression
        case 256:  // filterFields
        case 257:  // filterVal
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 121:  // projectionFieldname
        case 122:  // expressionFieldname
        case 123:  // stageAsUserFieldname
        case 124:  // filterFieldname
        case 125:  // argAsUserFieldname
        case 126:  // aggExprAsUserFieldname
        case 127:  // invariableUserFieldname
        case 128:  // idAsUserFieldname
        case 129:  // valueFieldname
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 102:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 112:  // "non-zero decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 101:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 113:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 115:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 114:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 103:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 100:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 111:  // "non-zero double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 109:  // "non-zero integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 110:  // "non-zero long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 130:  // projectField
        case 131:  // expressionField
        case 132:  // valueField
        case 133:  // filterField
        case 246:  // onErrorArg
        case 247:  // onNullArg
        case 248:  // formatArg
        case 249:  // timezoneArg
        case 250:  // charsArg
        case 251:  // optionsArg
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 252:  // expressions
        case 253:  // values
        case 254:  // exprZeroToTwo
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
        case 99:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 106:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 108:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 105:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 104:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 107:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 134:  // dbPointer
        case 135:  // javascript
        case 136:  // symbol
        case 137:  // javascriptWScope
        case 138:  // int
        case 139:  // timestamp
        case 140:  // long
        case 141:  // double
        case 142:  // decimal
        case 143:  // minKey
        case 144:  // maxKey
        case 145:  // value
        case 146:  // string
        case 147:  // fieldPath
        case 148:  // binary
        case 149:  // undefined
        case 150:  // objectId
        case 151:  // bool
        case 152:  // date
        case 153:  // null
        case 154:  // regex
        case 155:  // simpleValue
        case 156:  // compoundValue
        case 157:  // valueArray
        case 158:  // valueObject
        case 159:  // valueFields
        case 160:  // variable
        case 161:  // stageList
        case 162:  // stage
        case 163:  // inhibitOptimization
        case 164:  // unionWith
        case 165:  // skip
        case 166:  // limit
        case 167:  // project
        case 168:  // sample
        case 169:  // projectFields
        case 170:  // projection
        case 171:  // num
        case 172:  // expression
        case 173:  // compoundExpression
        case 174:  // exprFixedTwoArg
        case 175:  // expressionArray
        case 176:  // expressionObject
        case 177:  // expressionFields
        case 178:  // maths
        case 179:  // add
        case 180:  // atan2
        case 181:  // boolExps
        case 182:  // and
        case 183:  // or
        case 184:  // not
        case 185:  // literalEscapes
        case 186:  // const
        case 187:  // literal
        case 188:  // stringExps
        case 189:  // concat
        case 190:  // dateFromString
        case 191:  // dateToString
        case 192:  // indexOfBytes
        case 193:  // indexOfCP
        case 194:  // ltrim
        case 195:  // regexFind
        case 196:  // regexFindAll
        case 197:  // regexMatch
        case 198:  // regexArgs
        case 199:  // replaceOne
        case 200:  // replaceAll
        case 201:  // rtrim
        case 202:  // split
        case 203:  // strLenBytes
        case 204:  // strLenCP
        case 205:  // strcasecmp
        case 206:  // substr
        case 207:  // substrBytes
        case 208:  // substrCP
        case 209:  // toLower
        case 210:  // toUpper
        case 211:  // trim
        case 212:  // compExprs
        case 213:  // cmp
        case 214:  // eq
        case 215:  // gt
        case 216:  // gte
        case 217:  // lt
        case 218:  // lte
        case 219:  // ne
        case 220:  // typeExpression
        case 221:  // convert
        case 222:  // toBool
        case 223:  // toDate
        case 224:  // toDecimal
        case 225:  // toDouble
        case 226:  // toInt
        case 227:  // toLong
        case 228:  // toObjectId
        case 229:  // toString
        case 230:  // type
        case 231:  // abs
        case 232:  // ceil
        case 233:  // divide
        case 234:  // exponent
        case 235:  // floor
        case 236:  // ln
        case 237:  // log
        case 238:  // logten
        case 239:  // mod
        case 240:  // multiply
        case 241:  // pow
        case 242:  // round
        case 243:  // sqrt
        case 244:  // subtract
        case 245:  // trunc
        case 255:  // matchExpression
        case 256:  // filterFields
        case 257:  // filterVal
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 121:  // projectionFieldname
        case 122:  // expressionFieldname
        case 123:  // stageAsUserFieldname
        case 124:  // filterFieldname
        case 125:  // argAsUserFieldname
        case 126:  // aggExprAsUserFieldname
        case 127:  // invariableUserFieldname
        case 128:  // idAsUserFieldname
        case 129:  // valueFieldname
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 102:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 112:  // "non-zero decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 101:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 113:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 115:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 114:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 103:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 100:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 111:  // "non-zero double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 109:  // "non-zero integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 110:  // "non-zero long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 130:  // projectField
        case 131:  // expressionField
        case 132:  // valueField
        case 133:  // filterField
        case 246:  // onErrorArg
        case 247:  // onNullArg
        case 248:  // formatArg
        case 249:  // timezoneArg
        case 250:  // charsArg
        case 251:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 252:  // expressions
        case 253:  // values
        case 254:  // exprZeroToTwo
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
        case 99:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 106:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 108:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 105:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 104:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 107:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 134:  // dbPointer
        case 135:  // javascript
        case 136:  // symbol
        case 137:  // javascriptWScope
        case 138:  // int
        case 139:  // timestamp
        case 140:  // long
        case 141:  // double
        case 142:  // decimal
        case 143:  // minKey
        case 144:  // maxKey
        case 145:  // value
        case 146:  // string
        case 147:  // fieldPath
        case 148:  // binary
        case 149:  // undefined
        case 150:  // objectId
        case 151:  // bool
        case 152:  // date
        case 153:  // null
        case 154:  // regex
        case 155:  // simpleValue
        case 156:  // compoundValue
        case 157:  // valueArray
        case 158:  // valueObject
        case 159:  // valueFields
        case 160:  // variable
        case 161:  // stageList
        case 162:  // stage
        case 163:  // inhibitOptimization
        case 164:  // unionWith
        case 165:  // skip
        case 166:  // limit
        case 167:  // project
        case 168:  // sample
        case 169:  // projectFields
        case 170:  // projection
        case 171:  // num
        case 172:  // expression
        case 173:  // compoundExpression
        case 174:  // exprFixedTwoArg
        case 175:  // expressionArray
        case 176:  // expressionObject
        case 177:  // expressionFields
        case 178:  // maths
        case 179:  // add
        case 180:  // atan2
        case 181:  // boolExps
        case 182:  // and
        case 183:  // or
        case 184:  // not
        case 185:  // literalEscapes
        case 186:  // const
        case 187:  // literal
        case 188:  // stringExps
        case 189:  // concat
        case 190:  // dateFromString
        case 191:  // dateToString
        case 192:  // indexOfBytes
        case 193:  // indexOfCP
        case 194:  // ltrim
        case 195:  // regexFind
        case 196:  // regexFindAll
        case 197:  // regexMatch
        case 198:  // regexArgs
        case 199:  // replaceOne
        case 200:  // replaceAll
        case 201:  // rtrim
        case 202:  // split
        case 203:  // strLenBytes
        case 204:  // strLenCP
        case 205:  // strcasecmp
        case 206:  // substr
        case 207:  // substrBytes
        case 208:  // substrCP
        case 209:  // toLower
        case 210:  // toUpper
        case 211:  // trim
        case 212:  // compExprs
        case 213:  // cmp
        case 214:  // eq
        case 215:  // gt
        case 216:  // gte
        case 217:  // lt
        case 218:  // lte
        case 219:  // ne
        case 220:  // typeExpression
        case 221:  // convert
        case 222:  // toBool
        case 223:  // toDate
        case 224:  // toDecimal
        case 225:  // toDouble
        case 226:  // toInt
        case 227:  // toLong
        case 228:  // toObjectId
        case 229:  // toString
        case 230:  // type
        case 231:  // abs
        case 232:  // ceil
        case 233:  // divide
        case 234:  // exponent
        case 235:  // floor
        case 236:  // ln
        case 237:  // log
        case 238:  // logten
        case 239:  // mod
        case 240:  // multiply
        case 241:  // pow
        case 242:  // round
        case 243:  // sqrt
        case 244:  // subtract
        case 245:  // trunc
        case 255:  // matchExpression
        case 256:  // filterFields
        case 257:  // filterVal
            value.copy<CNode>(that.value);
            break;

        case 121:  // projectionFieldname
        case 122:  // expressionFieldname
        case 123:  // stageAsUserFieldname
        case 124:  // filterFieldname
        case 125:  // argAsUserFieldname
        case 126:  // aggExprAsUserFieldname
        case 127:  // invariableUserFieldname
        case 128:  // idAsUserFieldname
        case 129:  // valueFieldname
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 102:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 112:  // "non-zero decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 101:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 113:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 115:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 114:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 103:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 100:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 111:  // "non-zero double"
            value.copy<double>(that.value);
            break;

        case 109:  // "non-zero integer"
            value.copy<int>(that.value);
            break;

        case 110:  // "non-zero long"
            value.copy<long long>(that.value);
            break;

        case 130:  // projectField
        case 131:  // expressionField
        case 132:  // valueField
        case 133:  // filterField
        case 246:  // onErrorArg
        case 247:  // onNullArg
        case 248:  // formatArg
        case 249:  // timezoneArg
        case 250:  // charsArg
        case 251:  // optionsArg
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
            value.copy<std::string>(that.value);
            break;

        case 252:  // expressions
        case 253:  // values
        case 254:  // exprZeroToTwo
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
        case 99:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 106:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 108:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 105:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 104:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 107:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 134:  // dbPointer
        case 135:  // javascript
        case 136:  // symbol
        case 137:  // javascriptWScope
        case 138:  // int
        case 139:  // timestamp
        case 140:  // long
        case 141:  // double
        case 142:  // decimal
        case 143:  // minKey
        case 144:  // maxKey
        case 145:  // value
        case 146:  // string
        case 147:  // fieldPath
        case 148:  // binary
        case 149:  // undefined
        case 150:  // objectId
        case 151:  // bool
        case 152:  // date
        case 153:  // null
        case 154:  // regex
        case 155:  // simpleValue
        case 156:  // compoundValue
        case 157:  // valueArray
        case 158:  // valueObject
        case 159:  // valueFields
        case 160:  // variable
        case 161:  // stageList
        case 162:  // stage
        case 163:  // inhibitOptimization
        case 164:  // unionWith
        case 165:  // skip
        case 166:  // limit
        case 167:  // project
        case 168:  // sample
        case 169:  // projectFields
        case 170:  // projection
        case 171:  // num
        case 172:  // expression
        case 173:  // compoundExpression
        case 174:  // exprFixedTwoArg
        case 175:  // expressionArray
        case 176:  // expressionObject
        case 177:  // expressionFields
        case 178:  // maths
        case 179:  // add
        case 180:  // atan2
        case 181:  // boolExps
        case 182:  // and
        case 183:  // or
        case 184:  // not
        case 185:  // literalEscapes
        case 186:  // const
        case 187:  // literal
        case 188:  // stringExps
        case 189:  // concat
        case 190:  // dateFromString
        case 191:  // dateToString
        case 192:  // indexOfBytes
        case 193:  // indexOfCP
        case 194:  // ltrim
        case 195:  // regexFind
        case 196:  // regexFindAll
        case 197:  // regexMatch
        case 198:  // regexArgs
        case 199:  // replaceOne
        case 200:  // replaceAll
        case 201:  // rtrim
        case 202:  // split
        case 203:  // strLenBytes
        case 204:  // strLenCP
        case 205:  // strcasecmp
        case 206:  // substr
        case 207:  // substrBytes
        case 208:  // substrCP
        case 209:  // toLower
        case 210:  // toUpper
        case 211:  // trim
        case 212:  // compExprs
        case 213:  // cmp
        case 214:  // eq
        case 215:  // gt
        case 216:  // gte
        case 217:  // lt
        case 218:  // lte
        case 219:  // ne
        case 220:  // typeExpression
        case 221:  // convert
        case 222:  // toBool
        case 223:  // toDate
        case 224:  // toDecimal
        case 225:  // toDouble
        case 226:  // toInt
        case 227:  // toLong
        case 228:  // toObjectId
        case 229:  // toString
        case 230:  // type
        case 231:  // abs
        case 232:  // ceil
        case 233:  // divide
        case 234:  // exponent
        case 235:  // floor
        case 236:  // ln
        case 237:  // log
        case 238:  // logten
        case 239:  // mod
        case 240:  // multiply
        case 241:  // pow
        case 242:  // round
        case 243:  // sqrt
        case 244:  // subtract
        case 245:  // trunc
        case 255:  // matchExpression
        case 256:  // filterFields
        case 257:  // filterVal
            value.move<CNode>(that.value);
            break;

        case 121:  // projectionFieldname
        case 122:  // expressionFieldname
        case 123:  // stageAsUserFieldname
        case 124:  // filterFieldname
        case 125:  // argAsUserFieldname
        case 126:  // aggExprAsUserFieldname
        case 127:  // invariableUserFieldname
        case 128:  // idAsUserFieldname
        case 129:  // valueFieldname
            value.move<CNode::Fieldname>(that.value);
            break;

        case 102:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 112:  // "non-zero decimal"
            value.move<Decimal128>(that.value);
            break;

        case 101:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 113:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 115:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 114:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 103:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 100:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 111:  // "non-zero double"
            value.move<double>(that.value);
            break;

        case 109:  // "non-zero integer"
            value.move<int>(that.value);
            break;

        case 110:  // "non-zero long"
            value.move<long long>(that.value);
            break;

        case 130:  // projectField
        case 131:  // expressionField
        case 132:  // valueField
        case 133:  // filterField
        case 246:  // onErrorArg
        case 247:  // onNullArg
        case 248:  // formatArg
        case 249:  // timezoneArg
        case 250:  // charsArg
        case 251:  // optionsArg
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 97:   // "fieldname"
        case 98:   // "string"
        case 116:  // "$-prefixed string"
        case 117:  // "$$-prefixed string"
            value.move<std::string>(that.value);
            break;

        case 252:  // expressions
        case 253:  // values
        case 254:  // exprZeroToTwo
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
                case 99:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 106:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 108:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 105:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 104:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 107:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 134:  // dbPointer
                case 135:  // javascript
                case 136:  // symbol
                case 137:  // javascriptWScope
                case 138:  // int
                case 139:  // timestamp
                case 140:  // long
                case 141:  // double
                case 142:  // decimal
                case 143:  // minKey
                case 144:  // maxKey
                case 145:  // value
                case 146:  // string
                case 147:  // fieldPath
                case 148:  // binary
                case 149:  // undefined
                case 150:  // objectId
                case 151:  // bool
                case 152:  // date
                case 153:  // null
                case 154:  // regex
                case 155:  // simpleValue
                case 156:  // compoundValue
                case 157:  // valueArray
                case 158:  // valueObject
                case 159:  // valueFields
                case 160:  // variable
                case 161:  // stageList
                case 162:  // stage
                case 163:  // inhibitOptimization
                case 164:  // unionWith
                case 165:  // skip
                case 166:  // limit
                case 167:  // project
                case 168:  // sample
                case 169:  // projectFields
                case 170:  // projection
                case 171:  // num
                case 172:  // expression
                case 173:  // compoundExpression
                case 174:  // exprFixedTwoArg
                case 175:  // expressionArray
                case 176:  // expressionObject
                case 177:  // expressionFields
                case 178:  // maths
                case 179:  // add
                case 180:  // atan2
                case 181:  // boolExps
                case 182:  // and
                case 183:  // or
                case 184:  // not
                case 185:  // literalEscapes
                case 186:  // const
                case 187:  // literal
                case 188:  // stringExps
                case 189:  // concat
                case 190:  // dateFromString
                case 191:  // dateToString
                case 192:  // indexOfBytes
                case 193:  // indexOfCP
                case 194:  // ltrim
                case 195:  // regexFind
                case 196:  // regexFindAll
                case 197:  // regexMatch
                case 198:  // regexArgs
                case 199:  // replaceOne
                case 200:  // replaceAll
                case 201:  // rtrim
                case 202:  // split
                case 203:  // strLenBytes
                case 204:  // strLenCP
                case 205:  // strcasecmp
                case 206:  // substr
                case 207:  // substrBytes
                case 208:  // substrCP
                case 209:  // toLower
                case 210:  // toUpper
                case 211:  // trim
                case 212:  // compExprs
                case 213:  // cmp
                case 214:  // eq
                case 215:  // gt
                case 216:  // gte
                case 217:  // lt
                case 218:  // lte
                case 219:  // ne
                case 220:  // typeExpression
                case 221:  // convert
                case 222:  // toBool
                case 223:  // toDate
                case 224:  // toDecimal
                case 225:  // toDouble
                case 226:  // toInt
                case 227:  // toLong
                case 228:  // toObjectId
                case 229:  // toString
                case 230:  // type
                case 231:  // abs
                case 232:  // ceil
                case 233:  // divide
                case 234:  // exponent
                case 235:  // floor
                case 236:  // ln
                case 237:  // log
                case 238:  // logten
                case 239:  // mod
                case 240:  // multiply
                case 241:  // pow
                case 242:  // round
                case 243:  // sqrt
                case 244:  // subtract
                case 245:  // trunc
                case 255:  // matchExpression
                case 256:  // filterFields
                case 257:  // filterVal
                    yylhs.value.emplace<CNode>();
                    break;

                case 121:  // projectionFieldname
                case 122:  // expressionFieldname
                case 123:  // stageAsUserFieldname
                case 124:  // filterFieldname
                case 125:  // argAsUserFieldname
                case 126:  // aggExprAsUserFieldname
                case 127:  // invariableUserFieldname
                case 128:  // idAsUserFieldname
                case 129:  // valueFieldname
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 102:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 112:  // "non-zero decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 101:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 113:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 115:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 114:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 103:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 100:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 111:  // "non-zero double"
                    yylhs.value.emplace<double>();
                    break;

                case 109:  // "non-zero integer"
                    yylhs.value.emplace<int>();
                    break;

                case 110:  // "non-zero long"
                    yylhs.value.emplace<long long>();
                    break;

                case 130:  // projectField
                case 131:  // expressionField
                case 132:  // valueField
                case 133:  // filterField
                case 246:  // onErrorArg
                case 247:  // onNullArg
                case 248:  // formatArg
                case 249:  // timezoneArg
                case 250:  // charsArg
                case 251:  // optionsArg
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 97:   // "fieldname"
                case 98:   // "string"
                case 116:  // "$-prefixed string"
                case 117:  // "$$-prefixed string"
                    yylhs.value.emplace<std::string>();
                    break;

                case 252:  // expressions
                case 253:  // values
                case 254:  // exprZeroToTwo
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
#line 282 "pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1678 "pipeline_parser_gen.cpp"
                    break;

                    case 4:
#line 289 "pipeline_grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1686 "pipeline_parser_gen.cpp"
                    break;

                    case 5:
#line 295 "pipeline_grammar.yy"
                    {
                    }
#line 1692 "pipeline_parser_gen.cpp"
                    break;

                    case 6:
#line 296 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1700 "pipeline_parser_gen.cpp"
                    break;

                    case 7:
#line 304 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1706 "pipeline_parser_gen.cpp"
                    break;

                    case 9:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1712 "pipeline_parser_gen.cpp"
                    break;

                    case 10:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1718 "pipeline_parser_gen.cpp"
                    break;

                    case 11:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1724 "pipeline_parser_gen.cpp"
                    break;

                    case 12:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1730 "pipeline_parser_gen.cpp"
                    break;

                    case 13:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1736 "pipeline_parser_gen.cpp"
                    break;

                    case 14:
#line 307 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1742 "pipeline_parser_gen.cpp"
                    break;

                    case 15:
#line 310 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1754 "pipeline_parser_gen.cpp"
                    break;

                    case 16:
#line 320 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1762 "pipeline_parser_gen.cpp"
                    break;

                    case 17:
#line 326 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1775 "pipeline_parser_gen.cpp"
                    break;

                    case 18:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1781 "pipeline_parser_gen.cpp"
                    break;

                    case 19:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1787 "pipeline_parser_gen.cpp"
                    break;

                    case 20:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1793 "pipeline_parser_gen.cpp"
                    break;

                    case 21:
#line 336 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1799 "pipeline_parser_gen.cpp"
                    break;

                    case 22:
#line 340 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1807 "pipeline_parser_gen.cpp"
                    break;

                    case 23:
#line 345 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1815 "pipeline_parser_gen.cpp"
                    break;

                    case 24:
#line 350 "pipeline_grammar.yy"
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
                            // Pass the location of the $project token to the error reporting
                            // function.
                            error(yystack_[3].location, inclusion.getStatus().reason());
                    }
#line 1833 "pipeline_parser_gen.cpp"
                    break;

                    case 25:
#line 366 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1841 "pipeline_parser_gen.cpp"
                    break;

                    case 26:
#line 369 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1850 "pipeline_parser_gen.cpp"
                    break;

                    case 27:
#line 376 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1858 "pipeline_parser_gen.cpp"
                    break;

                    case 28:
#line 379 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1866 "pipeline_parser_gen.cpp"
                    break;

                    case 29:
#line 385 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1872 "pipeline_parser_gen.cpp"
                    break;

                    case 30:
#line 386 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1878 "pipeline_parser_gen.cpp"
                    break;

                    case 31:
#line 387 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1884 "pipeline_parser_gen.cpp"
                    break;

                    case 32:
#line 388 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1890 "pipeline_parser_gen.cpp"
                    break;

                    case 33:
#line 389 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1896 "pipeline_parser_gen.cpp"
                    break;

                    case 34:
#line 390 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1902 "pipeline_parser_gen.cpp"
                    break;

                    case 35:
#line 391 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1908 "pipeline_parser_gen.cpp"
                    break;

                    case 36:
#line 392 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1914 "pipeline_parser_gen.cpp"
                    break;

                    case 37:
#line 393 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1920 "pipeline_parser_gen.cpp"
                    break;

                    case 38:
#line 394 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1926 "pipeline_parser_gen.cpp"
                    break;

                    case 39:
#line 395 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1932 "pipeline_parser_gen.cpp"
                    break;

                    case 40:
#line 396 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 1940 "pipeline_parser_gen.cpp"
                    break;

                    case 41:
#line 399 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 1948 "pipeline_parser_gen.cpp"
                    break;

                    case 42:
#line 402 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 1956 "pipeline_parser_gen.cpp"
                    break;

                    case 43:
#line 405 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 1964 "pipeline_parser_gen.cpp"
                    break;

                    case 44:
#line 408 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 1972 "pipeline_parser_gen.cpp"
                    break;

                    case 45:
#line 411 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 1980 "pipeline_parser_gen.cpp"
                    break;

                    case 46:
#line 414 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 1988 "pipeline_parser_gen.cpp"
                    break;

                    case 47:
#line 417 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 1996 "pipeline_parser_gen.cpp"
                    break;

                    case 48:
#line 420 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2004 "pipeline_parser_gen.cpp"
                    break;

                    case 49:
#line 423 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2012 "pipeline_parser_gen.cpp"
                    break;

                    case 50:
#line 426 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2018 "pipeline_parser_gen.cpp"
                    break;

                    case 51:
#line 427 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2024 "pipeline_parser_gen.cpp"
                    break;

                    case 52:
#line 428 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2030 "pipeline_parser_gen.cpp"
                    break;

                    case 53:
#line 429 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            c_node_disambiguation::disambiguateCompoundProjection(
                                YY_MOVE(yystack_[0].value.as<CNode>()));
                        if (stdx::holds_alternative<CompoundInconsistentKey>(
                                yylhs.value.as<CNode>().payload))
                            // TODO SERVER-50498: error() instead of uasserting
                            uasserted(ErrorCodes::FailedToParse,
                                      "object project field cannot contain both inclusion and "
                                      "exclusion indicators");
                    }
#line 2041 "pipeline_parser_gen.cpp"
                    break;

                    case 54:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2047 "pipeline_parser_gen.cpp"
                    break;

                    case 55:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2053 "pipeline_parser_gen.cpp"
                    break;

                    case 56:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2059 "pipeline_parser_gen.cpp"
                    break;

                    case 57:
#line 438 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2065 "pipeline_parser_gen.cpp"
                    break;

                    case 58:
#line 442 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 2073 "pipeline_parser_gen.cpp"
                    break;

                    case 59:
#line 448 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2081 "pipeline_parser_gen.cpp"
                    break;

                    case 60:
#line 451 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2090 "pipeline_parser_gen.cpp"
                    break;

                    case 61:
#line 458 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2098 "pipeline_parser_gen.cpp"
                    break;

                    case 62:
#line 461 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2106 "pipeline_parser_gen.cpp"
                    break;

                    case 63:
#line 467 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2112 "pipeline_parser_gen.cpp"
                    break;

                    case 64:
#line 471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2118 "pipeline_parser_gen.cpp"
                    break;

                    case 65:
#line 471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2124 "pipeline_parser_gen.cpp"
                    break;

                    case 66:
#line 471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2130 "pipeline_parser_gen.cpp"
                    break;

                    case 67:
#line 471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2136 "pipeline_parser_gen.cpp"
                    break;

                    case 68:
#line 475 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2144 "pipeline_parser_gen.cpp"
                    break;

                    case 69:
#line 483 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2152 "pipeline_parser_gen.cpp"
                    break;

                    case 70:
#line 486 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2160 "pipeline_parser_gen.cpp"
                    break;

                    case 71:
#line 489 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2168 "pipeline_parser_gen.cpp"
                    break;

                    case 72:
#line 492 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2176 "pipeline_parser_gen.cpp"
                    break;

                    case 73:
#line 495 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2184 "pipeline_parser_gen.cpp"
                    break;

                    case 74:
#line 498 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2192 "pipeline_parser_gen.cpp"
                    break;

                    case 75:
#line 507 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2200 "pipeline_parser_gen.cpp"
                    break;

                    case 76:
#line 510 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2208 "pipeline_parser_gen.cpp"
                    break;

                    case 77:
#line 513 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2216 "pipeline_parser_gen.cpp"
                    break;

                    case 78:
#line 516 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2224 "pipeline_parser_gen.cpp"
                    break;

                    case 79:
#line 519 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2232 "pipeline_parser_gen.cpp"
                    break;

                    case 80:
#line 522 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2240 "pipeline_parser_gen.cpp"
                    break;

                    case 81:
#line 525 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2248 "pipeline_parser_gen.cpp"
                    break;

                    case 82:
#line 528 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2256 "pipeline_parser_gen.cpp"
                    break;

                    case 83:
#line 531 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2264 "pipeline_parser_gen.cpp"
                    break;

                    case 84:
#line 534 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2272 "pipeline_parser_gen.cpp"
                    break;

                    case 85:
#line 537 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2280 "pipeline_parser_gen.cpp"
                    break;

                    case 86:
#line 540 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2288 "pipeline_parser_gen.cpp"
                    break;

                    case 87:
#line 543 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2296 "pipeline_parser_gen.cpp"
                    break;

                    case 88:
#line 546 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2304 "pipeline_parser_gen.cpp"
                    break;

                    case 89:
#line 549 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2312 "pipeline_parser_gen.cpp"
                    break;

                    case 90:
#line 552 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2320 "pipeline_parser_gen.cpp"
                    break;

                    case 91:
#line 560 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2328 "pipeline_parser_gen.cpp"
                    break;

                    case 92:
#line 563 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2336 "pipeline_parser_gen.cpp"
                    break;

                    case 93:
#line 566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2344 "pipeline_parser_gen.cpp"
                    break;

                    case 94:
#line 569 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2352 "pipeline_parser_gen.cpp"
                    break;

                    case 95:
#line 572 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2360 "pipeline_parser_gen.cpp"
                    break;

                    case 96:
#line 575 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2368 "pipeline_parser_gen.cpp"
                    break;

                    case 97:
#line 578 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2376 "pipeline_parser_gen.cpp"
                    break;

                    case 98:
#line 581 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2384 "pipeline_parser_gen.cpp"
                    break;

                    case 99:
#line 584 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2392 "pipeline_parser_gen.cpp"
                    break;

                    case 100:
#line 587 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2400 "pipeline_parser_gen.cpp"
                    break;

                    case 101:
#line 590 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2408 "pipeline_parser_gen.cpp"
                    break;

                    case 102:
#line 593 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2416 "pipeline_parser_gen.cpp"
                    break;

                    case 103:
#line 596 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2424 "pipeline_parser_gen.cpp"
                    break;

                    case 104:
#line 599 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2432 "pipeline_parser_gen.cpp"
                    break;

                    case 105:
#line 602 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2440 "pipeline_parser_gen.cpp"
                    break;

                    case 106:
#line 605 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2448 "pipeline_parser_gen.cpp"
                    break;

                    case 107:
#line 608 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2456 "pipeline_parser_gen.cpp"
                    break;

                    case 108:
#line 611 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2464 "pipeline_parser_gen.cpp"
                    break;

                    case 109:
#line 614 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2472 "pipeline_parser_gen.cpp"
                    break;

                    case 110:
#line 617 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2480 "pipeline_parser_gen.cpp"
                    break;

                    case 111:
#line 620 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2488 "pipeline_parser_gen.cpp"
                    break;

                    case 112:
#line 623 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2496 "pipeline_parser_gen.cpp"
                    break;

                    case 113:
#line 626 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2504 "pipeline_parser_gen.cpp"
                    break;

                    case 114:
#line 629 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2512 "pipeline_parser_gen.cpp"
                    break;

                    case 115:
#line 632 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2520 "pipeline_parser_gen.cpp"
                    break;

                    case 116:
#line 635 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2528 "pipeline_parser_gen.cpp"
                    break;

                    case 117:
#line 638 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2536 "pipeline_parser_gen.cpp"
                    break;

                    case 118:
#line 641 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2544 "pipeline_parser_gen.cpp"
                    break;

                    case 119:
#line 644 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2552 "pipeline_parser_gen.cpp"
                    break;

                    case 120:
#line 647 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2560 "pipeline_parser_gen.cpp"
                    break;

                    case 121:
#line 650 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2568 "pipeline_parser_gen.cpp"
                    break;

                    case 122:
#line 653 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2576 "pipeline_parser_gen.cpp"
                    break;

                    case 123:
#line 656 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2584 "pipeline_parser_gen.cpp"
                    break;

                    case 124:
#line 659 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2592 "pipeline_parser_gen.cpp"
                    break;

                    case 125:
#line 662 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2600 "pipeline_parser_gen.cpp"
                    break;

                    case 126:
#line 665 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2608 "pipeline_parser_gen.cpp"
                    break;

                    case 127:
#line 668 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2616 "pipeline_parser_gen.cpp"
                    break;

                    case 128:
#line 671 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2624 "pipeline_parser_gen.cpp"
                    break;

                    case 129:
#line 674 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2632 "pipeline_parser_gen.cpp"
                    break;

                    case 130:
#line 677 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2640 "pipeline_parser_gen.cpp"
                    break;

                    case 131:
#line 680 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2648 "pipeline_parser_gen.cpp"
                    break;

                    case 132:
#line 683 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2656 "pipeline_parser_gen.cpp"
                    break;

                    case 133:
#line 686 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2664 "pipeline_parser_gen.cpp"
                    break;

                    case 134:
#line 689 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2672 "pipeline_parser_gen.cpp"
                    break;

                    case 135:
#line 692 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2680 "pipeline_parser_gen.cpp"
                    break;

                    case 136:
#line 695 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2688 "pipeline_parser_gen.cpp"
                    break;

                    case 137:
#line 698 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 2696 "pipeline_parser_gen.cpp"
                    break;

                    case 138:
#line 701 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 2704 "pipeline_parser_gen.cpp"
                    break;

                    case 139:
#line 704 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 2712 "pipeline_parser_gen.cpp"
                    break;

                    case 140:
#line 707 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 2720 "pipeline_parser_gen.cpp"
                    break;

                    case 141:
#line 710 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 2728 "pipeline_parser_gen.cpp"
                    break;

                    case 142:
#line 713 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 2736 "pipeline_parser_gen.cpp"
                    break;

                    case 143:
#line 716 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 2744 "pipeline_parser_gen.cpp"
                    break;

                    case 144:
#line 719 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 2752 "pipeline_parser_gen.cpp"
                    break;

                    case 145:
#line 722 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 2760 "pipeline_parser_gen.cpp"
                    break;

                    case 146:
#line 725 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 2768 "pipeline_parser_gen.cpp"
                    break;

                    case 147:
#line 728 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 2776 "pipeline_parser_gen.cpp"
                    break;

                    case 148:
#line 731 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 2784 "pipeline_parser_gen.cpp"
                    break;

                    case 149:
#line 734 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 2792 "pipeline_parser_gen.cpp"
                    break;

                    case 150:
#line 737 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 2800 "pipeline_parser_gen.cpp"
                    break;

                    case 151:
#line 740 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 2808 "pipeline_parser_gen.cpp"
                    break;

                    case 152:
#line 747 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2816 "pipeline_parser_gen.cpp"
                    break;

                    case 153:
#line 752 "pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 2828 "pipeline_parser_gen.cpp"
                    break;

                    case 154:
#line 760 "pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 2841 "pipeline_parser_gen.cpp"
                    break;

                    case 155:
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2849 "pipeline_parser_gen.cpp"
                    break;

                    case 156:
#line 775 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2857 "pipeline_parser_gen.cpp"
                    break;

                    case 157:
#line 781 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2865 "pipeline_parser_gen.cpp"
                    break;

                    case 158:
#line 787 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 2873 "pipeline_parser_gen.cpp"
                    break;

                    case 159:
#line 793 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 2881 "pipeline_parser_gen.cpp"
                    break;

                    case 160:
#line 799 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 2889 "pipeline_parser_gen.cpp"
                    break;

                    case 161:
#line 805 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 2897 "pipeline_parser_gen.cpp"
                    break;

                    case 162:
#line 811 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 2905 "pipeline_parser_gen.cpp"
                    break;

                    case 163:
#line 817 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 2913 "pipeline_parser_gen.cpp"
                    break;

                    case 164:
#line 823 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 2921 "pipeline_parser_gen.cpp"
                    break;

                    case 165:
#line 829 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 2929 "pipeline_parser_gen.cpp"
                    break;

                    case 166:
#line 835 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 2937 "pipeline_parser_gen.cpp"
                    break;

                    case 167:
#line 841 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 2945 "pipeline_parser_gen.cpp"
                    break;

                    case 168:
#line 847 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2953 "pipeline_parser_gen.cpp"
                    break;

                    case 169:
#line 850 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 2961 "pipeline_parser_gen.cpp"
                    break;

                    case 170:
#line 856 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2969 "pipeline_parser_gen.cpp"
                    break;

                    case 171:
#line 859 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 2977 "pipeline_parser_gen.cpp"
                    break;

                    case 172:
#line 865 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2985 "pipeline_parser_gen.cpp"
                    break;

                    case 173:
#line 868 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 2993 "pipeline_parser_gen.cpp"
                    break;

                    case 174:
#line 874 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3001 "pipeline_parser_gen.cpp"
                    break;

                    case 175:
#line 877 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3009 "pipeline_parser_gen.cpp"
                    break;

                    case 176:
#line 883 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3017 "pipeline_parser_gen.cpp"
                    break;

                    case 177:
#line 886 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3025 "pipeline_parser_gen.cpp"
                    break;

                    case 178:
#line 892 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3031 "pipeline_parser_gen.cpp"
                    break;

                    case 179:
#line 893 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3037 "pipeline_parser_gen.cpp"
                    break;

                    case 180:
#line 894 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3043 "pipeline_parser_gen.cpp"
                    break;

                    case 181:
#line 895 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3049 "pipeline_parser_gen.cpp"
                    break;

                    case 182:
#line 896 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3055 "pipeline_parser_gen.cpp"
                    break;

                    case 183:
#line 897 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3061 "pipeline_parser_gen.cpp"
                    break;

                    case 184:
#line 898 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3067 "pipeline_parser_gen.cpp"
                    break;

                    case 185:
#line 899 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3073 "pipeline_parser_gen.cpp"
                    break;

                    case 186:
#line 900 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3079 "pipeline_parser_gen.cpp"
                    break;

                    case 187:
#line 901 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3085 "pipeline_parser_gen.cpp"
                    break;

                    case 188:
#line 902 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3091 "pipeline_parser_gen.cpp"
                    break;

                    case 189:
#line 903 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3097 "pipeline_parser_gen.cpp"
                    break;

                    case 190:
#line 904 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3103 "pipeline_parser_gen.cpp"
                    break;

                    case 191:
#line 905 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3109 "pipeline_parser_gen.cpp"
                    break;

                    case 192:
#line 906 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3115 "pipeline_parser_gen.cpp"
                    break;

                    case 193:
#line 907 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3121 "pipeline_parser_gen.cpp"
                    break;

                    case 194:
#line 908 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3127 "pipeline_parser_gen.cpp"
                    break;

                    case 195:
#line 909 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3133 "pipeline_parser_gen.cpp"
                    break;

                    case 196:
#line 910 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3139 "pipeline_parser_gen.cpp"
                    break;

                    case 197:
#line 911 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3145 "pipeline_parser_gen.cpp"
                    break;

                    case 198:
#line 912 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3151 "pipeline_parser_gen.cpp"
                    break;

                    case 199:
#line 919 "pipeline_grammar.yy"
                    {
                    }
#line 3157 "pipeline_parser_gen.cpp"
                    break;

                    case 200:
#line 920 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3166 "pipeline_parser_gen.cpp"
                    break;

                    case 201:
#line 927 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3172 "pipeline_parser_gen.cpp"
                    break;

                    case 202:
#line 927 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3178 "pipeline_parser_gen.cpp"
                    break;

                    case 203:
#line 931 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3186 "pipeline_parser_gen.cpp"
                    break;

                    case 204:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3192 "pipeline_parser_gen.cpp"
                    break;

                    case 205:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3198 "pipeline_parser_gen.cpp"
                    break;

                    case 206:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3204 "pipeline_parser_gen.cpp"
                    break;

                    case 207:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3210 "pipeline_parser_gen.cpp"
                    break;

                    case 208:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3216 "pipeline_parser_gen.cpp"
                    break;

                    case 209:
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3222 "pipeline_parser_gen.cpp"
                    break;

                    case 210:
#line 937 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3228 "pipeline_parser_gen.cpp"
                    break;

                    case 211:
#line 937 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3234 "pipeline_parser_gen.cpp"
                    break;

                    case 212:
#line 943 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3242 "pipeline_parser_gen.cpp"
                    break;

                    case 213:
#line 951 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3250 "pipeline_parser_gen.cpp"
                    break;

                    case 214:
#line 957 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3258 "pipeline_parser_gen.cpp"
                    break;

                    case 215:
#line 960 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3267 "pipeline_parser_gen.cpp"
                    break;

                    case 216:
#line 967 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3275 "pipeline_parser_gen.cpp"
                    break;

                    case 217:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3281 "pipeline_parser_gen.cpp"
                    break;

                    case 218:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3287 "pipeline_parser_gen.cpp"
                    break;

                    case 219:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3293 "pipeline_parser_gen.cpp"
                    break;

                    case 220:
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3299 "pipeline_parser_gen.cpp"
                    break;

                    case 221:
#line 978 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3307 "pipeline_parser_gen.cpp"
                    break;

                    case 222:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3313 "pipeline_parser_gen.cpp"
                    break;

                    case 223:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3319 "pipeline_parser_gen.cpp"
                    break;

                    case 224:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3325 "pipeline_parser_gen.cpp"
                    break;

                    case 225:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3331 "pipeline_parser_gen.cpp"
                    break;

                    case 226:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3337 "pipeline_parser_gen.cpp"
                    break;

                    case 227:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3343 "pipeline_parser_gen.cpp"
                    break;

                    case 228:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3349 "pipeline_parser_gen.cpp"
                    break;

                    case 229:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3355 "pipeline_parser_gen.cpp"
                    break;

                    case 230:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3361 "pipeline_parser_gen.cpp"
                    break;

                    case 231:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3367 "pipeline_parser_gen.cpp"
                    break;

                    case 232:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3373 "pipeline_parser_gen.cpp"
                    break;

                    case 233:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3379 "pipeline_parser_gen.cpp"
                    break;

                    case 234:
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3385 "pipeline_parser_gen.cpp"
                    break;

                    case 235:
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3391 "pipeline_parser_gen.cpp"
                    break;

                    case 236:
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3397 "pipeline_parser_gen.cpp"
                    break;

                    case 237:
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3403 "pipeline_parser_gen.cpp"
                    break;

                    case 238:
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3409 "pipeline_parser_gen.cpp"
                    break;

                    case 239:
#line 989 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3418 "pipeline_parser_gen.cpp"
                    break;

                    case 240:
#line 996 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3427 "pipeline_parser_gen.cpp"
                    break;

                    case 241:
#line 1002 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3435 "pipeline_parser_gen.cpp"
                    break;

                    case 242:
#line 1007 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3443 "pipeline_parser_gen.cpp"
                    break;

                    case 243:
#line 1012 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3452 "pipeline_parser_gen.cpp"
                    break;

                    case 244:
#line 1018 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3460 "pipeline_parser_gen.cpp"
                    break;

                    case 245:
#line 1023 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3468 "pipeline_parser_gen.cpp"
                    break;

                    case 246:
#line 1028 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3476 "pipeline_parser_gen.cpp"
                    break;

                    case 247:
#line 1033 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3485 "pipeline_parser_gen.cpp"
                    break;

                    case 248:
#line 1039 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3493 "pipeline_parser_gen.cpp"
                    break;

                    case 249:
#line 1044 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3502 "pipeline_parser_gen.cpp"
                    break;

                    case 250:
#line 1050 "pipeline_grammar.yy"
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
#line 3514 "pipeline_parser_gen.cpp"
                    break;

                    case 251:
#line 1059 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3523 "pipeline_parser_gen.cpp"
                    break;

                    case 252:
#line 1065 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3532 "pipeline_parser_gen.cpp"
                    break;

                    case 253:
#line 1071 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3540 "pipeline_parser_gen.cpp"
                    break;

                    case 254:
#line 1076 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3549 "pipeline_parser_gen.cpp"
                    break;

                    case 255:
#line 1082 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3558 "pipeline_parser_gen.cpp"
                    break;

                    case 256:
#line 1088 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3564 "pipeline_parser_gen.cpp"
                    break;

                    case 257:
#line 1088 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3570 "pipeline_parser_gen.cpp"
                    break;

                    case 258:
#line 1088 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3576 "pipeline_parser_gen.cpp"
                    break;

                    case 259:
#line 1092 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3585 "pipeline_parser_gen.cpp"
                    break;

                    case 260:
#line 1099 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3594 "pipeline_parser_gen.cpp"
                    break;

                    case 261:
#line 1106 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3603 "pipeline_parser_gen.cpp"
                    break;

                    case 262:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3609 "pipeline_parser_gen.cpp"
                    break;

                    case 263:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3615 "pipeline_parser_gen.cpp"
                    break;

                    case 264:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3621 "pipeline_parser_gen.cpp"
                    break;

                    case 265:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3627 "pipeline_parser_gen.cpp"
                    break;

                    case 266:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3633 "pipeline_parser_gen.cpp"
                    break;

                    case 267:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3639 "pipeline_parser_gen.cpp"
                    break;

                    case 268:
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3645 "pipeline_parser_gen.cpp"
                    break;

                    case 269:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3651 "pipeline_parser_gen.cpp"
                    break;

                    case 270:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3657 "pipeline_parser_gen.cpp"
                    break;

                    case 271:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3663 "pipeline_parser_gen.cpp"
                    break;

                    case 272:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3669 "pipeline_parser_gen.cpp"
                    break;

                    case 273:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3675 "pipeline_parser_gen.cpp"
                    break;

                    case 274:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3681 "pipeline_parser_gen.cpp"
                    break;

                    case 275:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3687 "pipeline_parser_gen.cpp"
                    break;

                    case 276:
#line 1114 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3693 "pipeline_parser_gen.cpp"
                    break;

                    case 277:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3699 "pipeline_parser_gen.cpp"
                    break;

                    case 278:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3705 "pipeline_parser_gen.cpp"
                    break;

                    case 279:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3711 "pipeline_parser_gen.cpp"
                    break;

                    case 280:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3717 "pipeline_parser_gen.cpp"
                    break;

                    case 281:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3723 "pipeline_parser_gen.cpp"
                    break;

                    case 282:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3729 "pipeline_parser_gen.cpp"
                    break;

                    case 283:
#line 1115 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3735 "pipeline_parser_gen.cpp"
                    break;

                    case 284:
#line 1119 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3747 "pipeline_parser_gen.cpp"
                    break;

                    case 285:
#line 1129 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 3755 "pipeline_parser_gen.cpp"
                    break;

                    case 286:
#line 1132 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3763 "pipeline_parser_gen.cpp"
                    break;

                    case 287:
#line 1138 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 3771 "pipeline_parser_gen.cpp"
                    break;

                    case 288:
#line 1141 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3779 "pipeline_parser_gen.cpp"
                    break;

                    case 289:
#line 1148 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateFromString,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateStringArg,
                                  YY_MOVE(yystack_[6].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[4].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3789 "pipeline_parser_gen.cpp"
                    break;

                    case 290:
#line 1157 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateToString,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[5].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[4].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3799 "pipeline_parser_gen.cpp"
                    break;

                    case 291:
#line 1165 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 3807 "pipeline_parser_gen.cpp"
                    break;

                    case 292:
#line 1168 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3815 "pipeline_parser_gen.cpp"
                    break;

                    case 293:
#line 1171 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3823 "pipeline_parser_gen.cpp"
                    break;

                    case 294:
#line 1178 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3835 "pipeline_parser_gen.cpp"
                    break;

                    case 295:
#line 1189 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3847 "pipeline_parser_gen.cpp"
                    break;

                    case 296:
#line 1199 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 3855 "pipeline_parser_gen.cpp"
                    break;

                    case 297:
#line 1202 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3863 "pipeline_parser_gen.cpp"
                    break;

                    case 298:
#line 1208 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3873 "pipeline_parser_gen.cpp"
                    break;

                    case 299:
#line 1216 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3883 "pipeline_parser_gen.cpp"
                    break;

                    case 300:
#line 1224 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 3893 "pipeline_parser_gen.cpp"
                    break;

                    case 301:
#line 1232 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 3901 "pipeline_parser_gen.cpp"
                    break;

                    case 302:
#line 1235 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3909 "pipeline_parser_gen.cpp"
                    break;

                    case 303:
#line 1240 "pipeline_grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 3921 "pipeline_parser_gen.cpp"
                    break;

                    case 304:
#line 1249 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3929 "pipeline_parser_gen.cpp"
                    break;

                    case 305:
#line 1255 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3937 "pipeline_parser_gen.cpp"
                    break;

                    case 306:
#line 1261 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3945 "pipeline_parser_gen.cpp"
                    break;

                    case 307:
#line 1268 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 3956 "pipeline_parser_gen.cpp"
                    break;

                    case 308:
#line 1278 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 3967 "pipeline_parser_gen.cpp"
                    break;

                    case 309:
#line 1287 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3976 "pipeline_parser_gen.cpp"
                    break;

                    case 310:
#line 1294 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3985 "pipeline_parser_gen.cpp"
                    break;

                    case 311:
#line 1301 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3994 "pipeline_parser_gen.cpp"
                    break;

                    case 312:
#line 1309 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4003 "pipeline_parser_gen.cpp"
                    break;

                    case 313:
#line 1317 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4012 "pipeline_parser_gen.cpp"
                    break;

                    case 314:
#line 1325 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4021 "pipeline_parser_gen.cpp"
                    break;

                    case 315:
#line 1333 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4030 "pipeline_parser_gen.cpp"
                    break;

                    case 316:
#line 1340 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4038 "pipeline_parser_gen.cpp"
                    break;

                    case 317:
#line 1346 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4046 "pipeline_parser_gen.cpp"
                    break;

                    case 318:
#line 1352 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4052 "pipeline_parser_gen.cpp"
                    break;

                    case 319:
#line 1352 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4058 "pipeline_parser_gen.cpp"
                    break;

                    case 320:
#line 1356 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4067 "pipeline_parser_gen.cpp"
                    break;

                    case 321:
#line 1363 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4076 "pipeline_parser_gen.cpp"
                    break;

                    case 322:
#line 1370 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4082 "pipeline_parser_gen.cpp"
                    break;

                    case 323:
#line 1370 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4088 "pipeline_parser_gen.cpp"
                    break;

                    case 324:
#line 1374 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4094 "pipeline_parser_gen.cpp"
                    break;

                    case 325:
#line 1374 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4100 "pipeline_parser_gen.cpp"
                    break;

                    case 326:
#line 1378 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4108 "pipeline_parser_gen.cpp"
                    break;

                    case 327:
#line 1384 "pipeline_grammar.yy"
                    {
                    }
#line 4114 "pipeline_parser_gen.cpp"
                    break;

                    case 328:
#line 1385 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4123 "pipeline_parser_gen.cpp"
                    break;

                    case 329:
#line 1392 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4131 "pipeline_parser_gen.cpp"
                    break;

                    case 330:
#line 1398 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4139 "pipeline_parser_gen.cpp"
                    break;

                    case 331:
#line 1401 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4148 "pipeline_parser_gen.cpp"
                    break;

                    case 332:
#line 1408 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4156 "pipeline_parser_gen.cpp"
                    break;

                    case 333:
#line 1415 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4162 "pipeline_parser_gen.cpp"
                    break;

                    case 334:
#line 1416 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4168 "pipeline_parser_gen.cpp"
                    break;

                    case 335:
#line 1417 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4174 "pipeline_parser_gen.cpp"
                    break;

                    case 336:
#line 1418 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4180 "pipeline_parser_gen.cpp"
                    break;

                    case 337:
#line 1419 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4186 "pipeline_parser_gen.cpp"
                    break;

                    case 338:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4192 "pipeline_parser_gen.cpp"
                    break;

                    case 339:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4198 "pipeline_parser_gen.cpp"
                    break;

                    case 340:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4204 "pipeline_parser_gen.cpp"
                    break;

                    case 341:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4210 "pipeline_parser_gen.cpp"
                    break;

                    case 342:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4216 "pipeline_parser_gen.cpp"
                    break;

                    case 343:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4222 "pipeline_parser_gen.cpp"
                    break;

                    case 344:
#line 1422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4228 "pipeline_parser_gen.cpp"
                    break;

                    case 345:
#line 1424 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4237 "pipeline_parser_gen.cpp"
                    break;

                    case 346:
#line 1429 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4246 "pipeline_parser_gen.cpp"
                    break;

                    case 347:
#line 1434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4255 "pipeline_parser_gen.cpp"
                    break;

                    case 348:
#line 1439 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4264 "pipeline_parser_gen.cpp"
                    break;

                    case 349:
#line 1444 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4273 "pipeline_parser_gen.cpp"
                    break;

                    case 350:
#line 1449 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4282 "pipeline_parser_gen.cpp"
                    break;

                    case 351:
#line 1454 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4291 "pipeline_parser_gen.cpp"
                    break;

                    case 352:
#line 1460 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4297 "pipeline_parser_gen.cpp"
                    break;

                    case 353:
#line 1461 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4303 "pipeline_parser_gen.cpp"
                    break;

                    case 354:
#line 1462 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4309 "pipeline_parser_gen.cpp"
                    break;

                    case 355:
#line 1463 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4315 "pipeline_parser_gen.cpp"
                    break;

                    case 356:
#line 1464 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4321 "pipeline_parser_gen.cpp"
                    break;

                    case 357:
#line 1465 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4327 "pipeline_parser_gen.cpp"
                    break;

                    case 358:
#line 1466 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4333 "pipeline_parser_gen.cpp"
                    break;

                    case 359:
#line 1467 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4339 "pipeline_parser_gen.cpp"
                    break;

                    case 360:
#line 1468 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4345 "pipeline_parser_gen.cpp"
                    break;

                    case 361:
#line 1469 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4351 "pipeline_parser_gen.cpp"
                    break;

                    case 362:
#line 1474 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4359 "pipeline_parser_gen.cpp"
                    break;

                    case 363:
#line 1477 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4367 "pipeline_parser_gen.cpp"
                    break;

                    case 364:
#line 1484 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4375 "pipeline_parser_gen.cpp"
                    break;

                    case 365:
#line 1487 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4383 "pipeline_parser_gen.cpp"
                    break;

                    case 366:
#line 1494 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::convert,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::toArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4394 "pipeline_parser_gen.cpp"
                    break;

                    case 367:
#line 1503 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4402 "pipeline_parser_gen.cpp"
                    break;

                    case 368:
#line 1508 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4410 "pipeline_parser_gen.cpp"
                    break;

                    case 369:
#line 1513 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4418 "pipeline_parser_gen.cpp"
                    break;

                    case 370:
#line 1518 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4426 "pipeline_parser_gen.cpp"
                    break;

                    case 371:
#line 1523 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4434 "pipeline_parser_gen.cpp"
                    break;

                    case 372:
#line 1528 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4442 "pipeline_parser_gen.cpp"
                    break;

                    case 373:
#line 1533 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4450 "pipeline_parser_gen.cpp"
                    break;

                    case 374:
#line 1538 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4458 "pipeline_parser_gen.cpp"
                    break;

                    case 375:
#line 1543 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4466 "pipeline_parser_gen.cpp"
                    break;


#line 4470 "pipeline_parser_gen.cpp"

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
            context yyctx(*this, yyla);
            std::string msg = yysyntax_error_(yyctx);
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

/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string PipelineParserGen::yytnamerr_(const char* yystr) {
    if (*yystr == '"') {
        std::string yyr;
        char const* yyp = yystr;

        for (;;)
            switch (*++yyp) {
                case '\'':
                case ',':
                    goto do_not_strip_quotes;

                case '\\':
                    if (*++yyp != '\\')
                        goto do_not_strip_quotes;
                    else
                        goto append;

                append:
                default:
                    yyr += *yyp;
                    break;

                case '"':
                    return yyr;
            }
    do_not_strip_quotes:;
    }

    return yystr;
}

std::string PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytnamerr_(yytname_[yysymbol]);
}


// PipelineParserGen::context.
PipelineParserGen::context::context(const PipelineParserGen& yyparser, const symbol_type& yyla)
    : yyparser_(yyparser), yyla_(yyla) {}

int PipelineParserGen::context::expected_tokens(symbol_kind_type yyarg[], int yyargn) const {
    // Actual number of expected tokens
    int yycount = 0;

    int yyn = yypact_[+yyparser_.yystack_[0].state];
    if (!yy_pact_value_is_default_(yyn)) {
        /* Start YYX at -YYN if negative to avoid negative indexes in
           YYCHECK.  In other words, skip the first -YYN actions for
           this state because they are default actions.  */
        int yyxbegin = yyn < 0 ? -yyn : 0;
        // Stay within bounds of both yycheck and yytname.
        int yychecklim = yylast_ - yyn + 1;
        int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
        for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck_[yyx + yyn] == yyx && yyx != symbol_kind::S_YYerror &&
                !yy_table_value_is_error_(yytable_[yyx + yyn])) {
                if (!yyarg)
                    ++yycount;
                else if (yycount == yyargn)
                    return 0;
                else
                    yyarg[yycount++] = YY_CAST(symbol_kind_type, yyx);
            }
    }

    if (yyarg && yycount == 0 && 0 < yyargn)
        yyarg[0] = symbol_kind::S_YYEMPTY;
    return yycount;
}


int PipelineParserGen::yy_syntax_error_arguments_(const context& yyctx,
                                                  symbol_kind_type yyarg[],
                                                  int yyargn) const {
    /* There are many possibilities here to consider:
       - If this state is a consistent state with a default action, then
         the only way this function was invoked is if the default action
         is an error action.  In that case, don't check for expected
         tokens because there are none.
       - The only way there can be no lookahead present (in yyla) is
         if this state is a consistent state with a default action.
         Thus, detecting the absence of a lookahead is sufficient to
         determine that there is no unexpected or expected token to
         report.  In that case, just report a simple "syntax error".
       - Don't assume there isn't a lookahead just because this state is
         a consistent state with a default action.  There might have
         been a previous inconsistent state, consistent state with a
         non-default action, or user semantic action that manipulated
         yyla.  (However, yyla is currently not documented for users.)
       - Of course, the expected token list depends on states to have
         correct lookahead information, and it depends on the parser not
         to perform extra reductions after fetching a lookahead from the
         scanner and before detecting a syntax error.  Thus, state merging
         (from LALR or IELR) and default reductions corrupt the expected
         token list.  However, the list is correct for canonical LR with
         one exception: it will still contain any token that will not be
         accepted due to an error action in a later state.
    */

    if (!yyctx.lookahead().empty()) {
        if (yyarg)
            yyarg[0] = yyctx.token();
        int yyn = yyctx.expected_tokens(yyarg ? yyarg + 1 : yyarg, yyargn - 1);
        return yyn + 1;
    }
    return 0;
}

// Generate an error message.
std::string PipelineParserGen::yysyntax_error_(const context& yyctx) const {
    // Its maximum.
    enum { YYARGS_MAX = 5 };
    // Arguments of yyformat.
    symbol_kind_type yyarg[YYARGS_MAX];
    int yycount = yy_syntax_error_arguments_(yyctx, yyarg, YYARGS_MAX);

    char const* yyformat = YY_NULLPTR;
    switch (yycount) {
#define YYCASE_(N, S) \
    case N:           \
        yyformat = S; \
        break
        default:  // Avoid compiler warnings.
            YYCASE_(0, YY_("syntax error"));
            YYCASE_(1, YY_("syntax error, unexpected %s"));
            YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
            YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
            YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
            YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

    std::string yyres;
    // Argument number.
    std::ptrdiff_t yyi = 0;
    for (char const* yyp = yyformat; *yyp; ++yyp)
        if (yyp[0] == '%' && yyp[1] == 's' && yyi < yycount) {
            yyres += symbol_name(yyarg[yyi++]);
            ++yyp;
        } else
            yyres += *yyp;
    return yyres;
}


const short PipelineParserGen::yypact_ninf_ = -580;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -71,  -52,  -44,  36,   -36,  -580, -580, -580, -580, -28,  18,   360,  -16,  55,   0,    10,
    55,   -580, 52,   -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, 625,  -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, 625,  -580, -580, -580, -580, 61,   -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, 82,   -580, 99,   33,   -36,  -580, -580, 625,  -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, 455,  55,   13,   -580, -580,
    625,  85,   550,  -580, 834,  834,  -580, -580, -580, -580, -580, 86,   109,  -580, -580, -580,
    -580, -580, -580, -580, -580, -580, 625,  -580, -580, -580, -580, -580, -580, -580, 645,  760,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -29,  -580, -580, 645,  -580,
    95,   645,  57,   57,   65,   645,  65,   70,   71,   -580, -580, -580, 73,   65,   645,  645,
    65,   65,   74,   75,   76,   645,  77,   645,  65,   65,   -580, 78,   81,   65,   83,   57,
    84,   -580, -580, -580, -580, -580, 93,   -580, 96,   645,  97,   645,  645,  100,  101,  106,
    107,  645,  645,  645,  645,  645,  645,  645,  645,  645,  645,  -580, 112,  645,  881,  121,
    -580, -580, 138,  152,  156,  645,  158,  167,  170,  645,  625,  195,  199,  201,  645,  174,
    175,  176,  178,  179,  645,  645,  625,  180,  645,  181,  182,  183,  215,  645,  645,  186,
    645,  189,  645,  190,  217,  191,  194,  221,  222,  645,  215,  645,  197,  645,  198,  200,
    645,  645,  645,  645,  202,  203,  207,  209,  210,  211,  212,  214,  216,  218,  215,  645,
    220,  -580, 645,  -580, -580, -580, -580, -580, -580, -580, -580, -580, 645,  -580, -580, -580,
    224,  225,  645,  645,  645,  645,  -580, -580, -580, -580, -580, 645,  645,  226,  -580, 645,
    -580, -580, -580, 645,  223,  645,  645,  -580, 227,  -580, 645,  -580, 645,  -580, -580, 645,
    645,  645,  241,  645,  -580, 645,  -580, -580, 645,  645,  645,  645,  -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, 245,  645,  -580, -580, 228,  230,  233,  251,  260,  260,
    237,  645,  645,  238,  240,  -580, 645,  242,  645,  243,  246,  258,  264,  266,  247,  645,
    249,  250,  645,  645,  645,  252,  645,  253,  -580, -580, -580, 645,  272,  645,  269,  269,
    254,  645,  256,  257,  -580, 259,  261,  262,  265,  -580, 263,  645,  276,  645,  645,  267,
    268,  270,  271,  273,  274,  280,  275,  281,  282,  -580, 645,  295,  -580, 645,  251,  272,
    -580, -580, 285,  286,  -580, 287,  -580, 288,  -580, -580, 645,  283,  284,  -580, 289,  -580,
    -580, 290,  291,  292,  -580, 294,  -580, -580, 645,  -580, 272,  296,  -580, -580, -580, -580,
    297,  645,  645,  -580, -580, -580, -580, -580, 298,  300,  301,  -580, 302,  305,  307,  308,
    -580, 309,  310,  -580, -580, -580, -580};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   5,   2,   59,  3,   1,   0,   0,   0,   0,   0,   0,   0,   0,   7,   0,
    9,   10,  11,  12,  13,  14,  4,   115, 91,  93,  86,  75,  85,  82,  89,  83,  78,  80,  81,
    88,  76,  87,  90,  77,  84,  79,  92,  116, 98,  130, 94,  105, 131, 132, 117, 58,  99,  118,
    119, 100, 101, 0,   133, 134, 95,  120, 121, 122, 102, 103, 135, 123, 124, 104, 97,  96,  125,
    136, 137, 138, 140, 139, 126, 141, 142, 127, 69,  72,  73,  74,  71,  70,  145, 143, 144, 146,
    147, 148, 128, 106, 107, 108, 109, 110, 111, 149, 112, 113, 151, 150, 129, 114, 68,  65,  0,
    66,  67,  64,  60,  0,   175, 173, 169, 171, 168, 170, 172, 174, 18,  19,  20,  21,  23,  25,
    0,   22,  0,   0,   5,   177, 176, 327, 330, 152, 155, 156, 157, 158, 159, 160, 161, 162, 163,
    164, 165, 166, 167, 153, 154, 187, 188, 189, 190, 191, 196, 192, 193, 194, 197, 198, 63,  178,
    179, 181, 182, 183, 195, 184, 185, 186, 322, 323, 324, 325, 180, 61,  62,  16,  0,   0,   0,
    8,   6,   327, 0,   0,   24,  0,   0,   55,  56,  57,  54,  26,  0,   0,   328, 326, 329, 221,
    334, 335, 336, 333, 337, 0,   331, 49,  48,  47,  45,  41,  43,  199, 214, 40,  42,  44,  46,
    36,  37,  38,  39,  50,  51,  52,  29,  30,  31,  32,  33,  34,  35,  27,  53,  204, 205, 206,
    222, 223, 207, 256, 257, 258, 208, 318, 319, 211, 262, 263, 264, 265, 266, 267, 268, 269, 270,
    271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 283, 282, 209, 338, 339, 340, 341, 342,
    343, 344, 210, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 224, 225, 226, 227, 228, 229,
    230, 231, 232, 233, 234, 235, 236, 237, 238, 28,  15,  0,   332, 201, 199, 202, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   7,   7,   7,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   7,   0,   0,   0,   0,   0,   0,   7,   7,   7,   7,   7,   0,   7,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   7,
    0,   0,   0,   0,   200, 212, 0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   296, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   296, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   296, 0,   0,   213, 0,   218, 219, 217, 220, 215,
    17,  241, 239, 259, 0,   240, 242, 345, 0,   0,   0,   0,   0,   0,   346, 244, 245, 347, 348,
    0,   0,   0,   246, 0,   248, 349, 350, 0,   0,   0,   0,   351, 0,   260, 0,   304, 0,   305,
    306, 0,   0,   0,   0,   0,   253, 0,   310, 311, 0,   0,   0,   0,   367, 368, 369, 370, 371,
    372, 316, 373, 374, 317, 0,   0,   375, 216, 0,   0,   0,   362, 285, 285, 0,   291, 291, 0,
    0,   297, 0,   0,   199, 0,   0,   301, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   203, 284, 320, 0,   364, 0,   287, 287, 0,   292, 0,   0,   321, 0,   0,   0,   0,   261,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   363, 0,   0,   286,
    0,   362, 364, 243, 293, 0,   0,   247, 0,   249, 0,   251, 302, 0,   0,   0,   252, 0,   309,
    312, 0,   0,   0,   254, 0,   255, 365, 0,   288, 364, 0,   294, 295, 298, 250, 0,   0,   0,
    299, 313, 314, 315, 300, 0,   0,   0,   303, 0,   0,   0,   0,   290, 0,   0,   366, 289, 308,
    307};

const short PipelineParserGen::yypgoto_[] = {
    -580, -580, -580, -180, -580, -178, -167, -177, -88,  -580, -580, -580, -580, -580, -147, -145,
    -135, -123, -5,   -117, 8,    -10,  9,    -112, -106, -136, -163, -580, -103, -101, -93,  -580,
    -83,  -81,  -79,  -59,  -580, -580, -580, -580, -580, 213,  -580, -580, -580, -580, -580, -580,
    -580, -580, 134,  -3,   -306, -60,  -202, -292, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -216, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580,
    -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -580, -242, -579,
    -176, -210, -408, -580, -316, 160,  -175, -580, -580, 244,  -580, -580, -17,  -580};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  197, 450, 112, 113, 114, 115, 116, 213, 214, 202, 455, 215, 117, 158, 159, 160, 161,
    162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 317,
    180, 181, 182, 194, 183, 10,  18,  19,  20,  21,  22,  23,  24,  187, 242, 131, 318, 319,
    390, 244, 245, 382, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259,
    260, 261, 262, 263, 264, 265, 419, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276,
    277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294,
    295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312,
    556, 587, 558, 590, 484, 572, 320, 193, 562, 7,   11,  184, 3,   5,   420, 136};

const short PipelineParserGen::yytable_[] = {
    135, 179, 384, 129, 192, 120, 129, 198, 127, 199, 201, 127, 619, 134, 209, 386, 210, 212, 498,
    391, 200, 128, 130, 4,   128, 130, 204, 211, 400, 401, 387, 388, 6,   235, 235, 407, 8,   409,
    518, 633, 9,   12,  13,  14,  15,  16,  17,  1,   2,   228, 228, 229, 229, 25,  179, 428, 192,
    430, 431, 417, 118, 230, 230, 436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 231, 231, 448,
    132, 464, 316, 232, 232, 179, 125, 460, 233, 233, 133, 119, 137, 120, 234, 234, 469, 236, 236,
    237, 237, 186, 475, 476, 121, 188, 479, 238, 238, 122, 189, 485, 486, 190, 488, 142, 490, 239,
    239, 240, 240, 241, 241, 497, 206, 499, 314, 501, 392, 315, 504, 505, 506, 507, 385, 399, 222,
    179, 402, 403, 243, 243, 421, 422, 389, 519, 410, 411, 521, 393, 394, 415, 398, 404, 405, 406,
    408, 413, 522, 179, 414, 456, 416, 418, 525, 526, 527, 528, 123, 124, 125, 126, 425, 529, 530,
    427, 429, 532, 457, 432, 433, 533, 129, 535, 536, 434, 435, 127, 538, 203, 539, 447, 458, 540,
    541, 542, 459, 544, 461, 545, 128, 130, 546, 547, 548, 549, 451, 462, 452, 453, 463, 466, 467,
    468, 470, 471, 472, 551, 473, 474, 478, 480, 481, 482, 568, 483, 487, 561, 561, 489, 491, 493,
    566, 492, 494, 495, 496, 500, 502, 534, 503, 576, 508, 509, 579, 580, 581, 510, 583, 511, 512,
    513, 514, 585, 515, 588, 516, 543, 517, 593, 520, 550, 465, 523, 524, 531, 537, 552, 555, 601,
    553, 603, 604, 554, 477, 557, 560, 571, 564, 565, 573, 567, 574, 569, 615, 570, 575, 617, 577,
    578, 586, 582, 584, 589, 592, 594, 595, 602, 454, 596, 624, 597, 598, 600, 599, 625, 626, 605,
    606, 383, 607, 608, 609, 610, 632, 612, 395, 396, 397, 611, 616, 613, 614, 636, 637, 620, 621,
    622, 623, 627, 628, 629, 630, 412, 631, 313, 634, 635, 638, 179, 639, 640, 641, 423, 424, 642,
    426, 643, 644, 645, 646, 179, 618, 591, 191, 559, 205, 0,   0,   563, 0,   185, 0,   0,   0,
    0,   446, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  0,   0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   54,  55,  56,
    57,  58,  59,  60,  61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,
    91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  0,   0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   195, 55,  56,
    57,  58,  59,  196, 61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,
    91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
    43,  44,  45,  0,   0,   46,  47,  48,  49,  50,  51,  52,  0,   53,  0,   0,   207, 55,  56,
    57,  58,  59,  208, 61,  62,  0,   63,  64,  65,  66,  0,   67,  68,  69,  70,  71,  72,  73,
    74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  0,   0,
    91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 138, 139, 0,   0,   0,   0,   0,   0,   0,   119, 0,   120, 0,   0,   0,   0,   0,
    0,   0,   0,   138, 139, 121, 0,   0,   0,   0,   122, 0,   119, 0,   120, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   121, 0,   0,   0,   0,   122, 0,   0,   0,   0,   140, 141, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   222, 223,
    0,   142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 123, 124, 125, 126, 153, 154, 155,
    156, 157, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 123, 124, 125, 126, 153, 154,
    155, 156, 157, 321, 322, 323, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   324, 0,   0,   325, 326, 327, 328, 329, 330, 331, 0,   332, 0,   0,   0,   333,
    334, 335, 336, 337, 0,   338, 339, 0,   340, 341, 342, 343, 0,   344, 345, 346, 347, 348, 349,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 0,   0,   0,   0,   0,   0,   0,
    0,   362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378, 379,
    380, 381, 216, 217, 0,   0,   0,   0,   0,   0,   0,   218, 0,   219, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   220, 0,   0,   0,   0,   221, 0,   0,   29,  30,  31,  32,  33,  34,
    35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  0,   0,   0,   0,   0,   0,   222, 223, 0,
    0,   0,   0,   0,   0,   449, 0,   0,   0,   0,   0,   208, 0,   0,   0,   0,   0,   0,   0,
    0,   142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 224, 225, 226, 227, 153, 154, 155,
    85,  86,  87,  88,  89,  90,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   111};

const short PipelineParserGen::yycheck_[] = {
    17,  60,  318, 13,  140, 34,  16,  187, 13,  187, 187, 16,  591, 16,  194, 321, 194, 194, 426,
    325, 187, 13,  13,  75,  16,  16,  189, 194, 334, 335, 322, 323, 76,  196, 197, 341, 0,   343,
    446, 618, 76,  69,  70,  71,  72,  73,  74,  118, 119, 196, 197, 196, 197, 35,  113, 361, 192,
    363, 364, 351, 76,  196, 197, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378, 196, 197, 381,
    76,  393, 214, 196, 197, 140, 111, 389, 196, 197, 76,  32,  36,  34,  196, 197, 398, 196, 197,
    196, 197, 36,  404, 405, 45,  19,  408, 196, 197, 50,  7,   413, 414, 76,  416, 98,  418, 196,
    197, 196, 197, 196, 197, 425, 35,  427, 36,  429, 326, 16,  432, 433, 434, 435, 35,  333, 75,
    192, 336, 337, 196, 197, 354, 355, 75,  447, 344, 345, 450, 75,  75,  349, 75,  75,  75,  75,
    75,  75,  460, 214, 75,  36,  75,  75,  466, 467, 468, 469, 109, 110, 111, 112, 75,  475, 476,
    75,  75,  479, 36,  75,  75,  483, 188, 485, 486, 75,  75,  188, 490, 188, 492, 75,  36,  495,
    496, 497, 36,  499, 36,  501, 188, 188, 504, 505, 506, 507, 382, 36,  382, 382, 36,  12,  9,
    8,   36,  36,  36,  519, 36,  36,  36,  36,  36,  36,  536, 6,   36,  529, 530, 36,  36,  36,
    534, 12,  36,  10,  10,  36,  36,  12,  36,  543, 36,  36,  546, 547, 548, 36,  550, 36,  36,
    36,  36,  555, 36,  557, 36,  12,  36,  561, 36,  12,  394, 35,  35,  35,  35,  35,  13,  571,
    36,  573, 574, 36,  406, 11,  35,  15,  36,  35,  12,  35,  12,  36,  586, 35,  35,  589, 35,
    35,  14,  35,  35,  20,  36,  35,  35,  17,  382, 36,  602, 36,  36,  36,  35,  18,  18,  36,
    36,  315, 36,  36,  35,  35,  616, 36,  329, 330, 331, 35,  21,  36,  36,  625, 626, 36,  36,
    36,  36,  36,  36,  36,  36,  346, 36,  197, 36,  36,  36,  394, 36,  36,  36,  356, 357, 36,
    359, 36,  36,  36,  36,  406, 590, 559, 137, 527, 192, -1,  -1,  530, -1,  113, -1,  -1,  -1,
    -1,  379, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,  57,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  -1,  -1,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  23,  24,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  23,  24,  45,  -1,  -1,  -1,  -1,  50,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  45,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,  75,  76,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,
    -1,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114,
    115, 116, 117, 3,   4,   5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  33,  -1,  -1,  -1,  37,
    38,  39,  40,  41,  -1,  43,  44,  -1,  46,  47,  48,  49,  -1,  51,  52,  53,  54,  55,  56,
    57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,
    95,  96,  23,  24,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  -1,  34,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  45,  -1,  -1,  -1,  -1,  50,  -1,  -1,  6,   7,   8,   9,   10,  11,
    12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,
    -1,  -1,  -1,  -1,  -1,  36,  -1,  -1,  -1,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    69,  70,  71,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  97};

const short PipelineParserGen::yystos_[] = {
    0,   118, 119, 258, 75,  259, 76,  255, 0,   76,  161, 256, 69,  70,  71,  72,  73,  74,  162,
    163, 164, 165, 166, 167, 168, 35,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  25,  26,  27,  28,  29,  30,  31,  33,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  46,  47,  48,  49,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  77,  78,  79,  80,
    81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  123, 124,
    125, 126, 127, 133, 76,  32,  34,  45,  50,  109, 110, 111, 112, 138, 140, 141, 142, 171, 76,
    76,  171, 260, 261, 36,  23,  24,  75,  76,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107,
    108, 113, 114, 115, 116, 117, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146,
    147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 160, 257, 257, 36,  169, 19,  7,
    76,  161, 145, 253, 159, 36,  42,  121, 123, 125, 126, 127, 130, 171, 146, 253, 35,  36,  42,
    123, 125, 126, 127, 128, 129, 132, 23,  24,  32,  34,  45,  50,  75,  76,  109, 110, 111, 112,
    134, 135, 136, 137, 139, 143, 144, 146, 148, 149, 150, 152, 153, 154, 170, 173, 175, 176, 178,
    179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
    218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236,
    237, 238, 239, 240, 241, 242, 243, 244, 245, 170, 36,  16,  145, 155, 172, 173, 252, 3,   4,
    5,   22,  25,  26,  27,  28,  29,  30,  31,  33,  37,  38,  39,  40,  41,  43,  44,  46,  47,
    48,  49,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,
    68,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,
    95,  96,  177, 141, 252, 35,  172, 175, 175, 75,  174, 172, 174, 75,  75,  260, 260, 260, 75,
    174, 172, 172, 174, 174, 75,  75,  75,  172, 75,  172, 174, 174, 260, 75,  75,  174, 75,  175,
    75,  198, 260, 198, 198, 260, 260, 75,  260, 75,  172, 75,  172, 172, 75,  75,  75,  75,  172,
    172, 172, 172, 172, 172, 172, 172, 172, 172, 260, 75,  172, 36,  122, 123, 125, 127, 128, 131,
    36,  36,  36,  36,  172, 36,  36,  36,  252, 145, 12,  9,   8,   172, 36,  36,  36,  36,  36,
    172, 172, 145, 36,  172, 36,  36,  36,  6,   250, 172, 172, 36,  172, 36,  172, 36,  12,  36,
    36,  10,  10,  172, 250, 172, 36,  172, 36,  36,  172, 172, 172, 172, 36,  36,  36,  36,  36,
    36,  36,  36,  36,  36,  250, 172, 36,  172, 172, 35,  35,  172, 172, 172, 172, 172, 172, 35,
    172, 172, 12,  172, 172, 35,  172, 172, 172, 172, 172, 12,  172, 172, 172, 172, 172, 172, 12,
    172, 35,  36,  36,  13,  246, 11,  248, 248, 35,  172, 254, 254, 36,  35,  172, 35,  252, 36,
    35,  15,  251, 12,  12,  35,  172, 35,  35,  172, 172, 172, 35,  172, 35,  172, 14,  247, 172,
    20,  249, 249, 36,  172, 35,  35,  36,  36,  36,  35,  36,  172, 17,  172, 172, 36,  36,  36,
    36,  35,  35,  35,  36,  36,  36,  172, 21,  172, 246, 247, 36,  36,  36,  36,  172, 18,  18,
    36,  36,  36,  36,  36,  172, 247, 36,  36,  172, 172, 36,  36,  36,  36,  36,  36,  36,  36,
    36};

const short PipelineParserGen::yyr1_[] = {
    0,   120, 258, 258, 259, 161, 161, 261, 260, 162, 162, 162, 162, 162, 162, 168, 163, 164, 171,
    171, 171, 171, 165, 166, 167, 169, 169, 130, 130, 170, 170, 170, 170, 170, 170, 170, 170, 170,
    170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 170, 121, 121, 121,
    121, 255, 256, 256, 133, 133, 257, 124, 124, 124, 124, 127, 123, 123, 123, 123, 123, 123, 125,
    125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 125, 126, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    146, 147, 160, 148, 149, 150, 152, 153, 154, 134, 135, 136, 137, 139, 143, 144, 138, 138, 140,
    140, 141, 141, 142, 142, 151, 151, 155, 155, 155, 155, 155, 155, 155, 155, 155, 155, 155, 155,
    155, 155, 155, 155, 155, 155, 155, 155, 155, 252, 252, 172, 172, 174, 173, 173, 173, 173, 173,
    173, 173, 173, 175, 176, 177, 177, 131, 122, 122, 122, 122, 128, 178, 178, 178, 178, 178, 178,
    178, 178, 178, 178, 178, 178, 178, 178, 178, 178, 178, 179, 180, 231, 232, 233, 234, 235, 236,
    237, 238, 239, 240, 241, 242, 243, 244, 245, 181, 181, 181, 182, 183, 184, 188, 188, 188, 188,
    188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 188, 189,
    248, 248, 249, 249, 190, 191, 254, 254, 254, 192, 193, 250, 250, 194, 201, 211, 251, 251, 198,
    195, 196, 197, 199, 200, 202, 203, 204, 205, 206, 207, 208, 209, 210, 185, 185, 186, 187, 145,
    145, 156, 156, 157, 253, 253, 158, 159, 159, 132, 129, 129, 129, 129, 129, 212, 212, 212, 212,
    212, 212, 212, 213, 214, 215, 216, 217, 218, 219, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 246, 246, 247, 247, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230};

const signed char PipelineParserGen::yyr2_[] = {
    0,  2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7,  1,  1, 1, 1, 2, 2, 4, 0, 2, 2, 2,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    3,  0, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 0, 2, 1, 1,
    4,  1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4, 7, 4, 7,  8,  7, 7, 4, 7, 7, 1, 1, 1, 4, 4,
    6,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 6, 0, 2, 0, 2, 11,
    10, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1,
    1,  6, 6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 4, 4, 4,
    4,  4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


#if YYDEBUG || 1
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "\"invalid token\"",
                                                   "ABS",
                                                   "ADD",
                                                   "AND",
                                                   "\"chars argument\"",
                                                   "\"coll argument\"",
                                                   "\"date argument\"",
                                                   "\"dateString argument\"",
                                                   "\"find argument\"",
                                                   "\"format argument\"",
                                                   "\"input argument\"",
                                                   "\"onError argument\"",
                                                   "\"onNull argument\"",
                                                   "\"options argument\"",
                                                   "\"pipeline argument\"",
                                                   "\"regex argument\"",
                                                   "\"replacement argument\"",
                                                   "\"size argument\"",
                                                   "\"timezone argument\"",
                                                   "\"to argument\"",
                                                   "ATAN2",
                                                   "\"false\"",
                                                   "\"true\"",
                                                   "CEIL",
                                                   "CMP",
                                                   "CONCAT",
                                                   "CONST_EXPR",
                                                   "CONVERT",
                                                   "DATE_FROM_STRING",
                                                   "DATE_TO_STRING",
                                                   "\"zero (decimal)\"",
                                                   "DIVIDE",
                                                   "\"zero (double)\"",
                                                   "\"end of array\"",
                                                   "\"end of object\"",
                                                   "EQ",
                                                   "EXPONENT",
                                                   "FLOOR",
                                                   "GT",
                                                   "GTE",
                                                   "ID",
                                                   "INDEX_OF_BYTES",
                                                   "INDEX_OF_CP",
                                                   "\"zero (int)\"",
                                                   "LITERAL",
                                                   "LN",
                                                   "LOG",
                                                   "LOGTEN",
                                                   "\"zero (long)\"",
                                                   "LT",
                                                   "LTE",
                                                   "LTRIM",
                                                   "MOD",
                                                   "MULTIPLY",
                                                   "NE",
                                                   "NOT",
                                                   "OR",
                                                   "POW",
                                                   "REGEX_FIND",
                                                   "REGEX_FIND_ALL",
                                                   "REGEX_MATCH",
                                                   "REPLACE_ALL",
                                                   "REPLACE_ONE",
                                                   "ROUND",
                                                   "RTRIM",
                                                   "SPLIT",
                                                   "SQRT",
                                                   "STAGE_INHIBIT_OPTIMIZATION",
                                                   "STAGE_LIMIT",
                                                   "STAGE_PROJECT",
                                                   "STAGE_SAMPLE",
                                                   "STAGE_SKIP",
                                                   "STAGE_UNION_WITH",
                                                   "\"array\"",
                                                   "\"object\"",
                                                   "STR_CASE_CMP",
                                                   "STR_LEN_BYTES",
                                                   "STR_LEN_CP",
                                                   "SUBSTR",
                                                   "SUBSTR_BYTES",
                                                   "SUBSTR_CP",
                                                   "SUBTRACT",
                                                   "TO_BOOL",
                                                   "TO_DATE",
                                                   "TO_DECIMAL",
                                                   "TO_DOUBLE",
                                                   "TO_INT",
                                                   "TO_LONG",
                                                   "TO_LOWER",
                                                   "TO_OBJECT_ID",
                                                   "TO_STRING",
                                                   "TO_UPPER",
                                                   "TRIM",
                                                   "TRUNC",
                                                   "TYPE",
                                                   "\"fieldname\"",
                                                   "\"string\"",
                                                   "\"BinData\"",
                                                   "\"undefined\"",
                                                   "\"ObjectID\"",
                                                   "\"Date\"",
                                                   "\"null\"",
                                                   "\"regex\"",
                                                   "\"dbPointer\"",
                                                   "\"Code\"",
                                                   "\"Symbol\"",
                                                   "\"CodeWScope\"",
                                                   "\"non-zero integer\"",
                                                   "\"non-zero long\"",
                                                   "\"non-zero double\"",
                                                   "\"non-zero decimal\"",
                                                   "\"Timestamp\"",
                                                   "\"minKey\"",
                                                   "\"maxKey\"",
                                                   "\"$-prefixed string\"",
                                                   "\"$$-prefixed string\"",
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
                                                   "fieldPath",
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
                                                   "variable",
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
                                                   "stringExps",
                                                   "concat",
                                                   "dateFromString",
                                                   "dateToString",
                                                   "indexOfBytes",
                                                   "indexOfCP",
                                                   "ltrim",
                                                   "regexFind",
                                                   "regexFindAll",
                                                   "regexMatch",
                                                   "regexArgs",
                                                   "replaceOne",
                                                   "replaceAll",
                                                   "rtrim",
                                                   "split",
                                                   "strLenBytes",
                                                   "strLenCP",
                                                   "strcasecmp",
                                                   "substr",
                                                   "substrBytes",
                                                   "substrCP",
                                                   "toLower",
                                                   "toUpper",
                                                   "trim",
                                                   "compExprs",
                                                   "cmp",
                                                   "eq",
                                                   "gt",
                                                   "gte",
                                                   "lt",
                                                   "lte",
                                                   "ne",
                                                   "typeExpression",
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
                                                   "formatArg",
                                                   "timezoneArg",
                                                   "charsArg",
                                                   "optionsArg",
                                                   "expressions",
                                                   "values",
                                                   "exprZeroToTwo",
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
    0,    281,  281,  282,  289,  295,  296,  304,  304,  307,  307,  307,  307,  307,  307,  310,
    320,  326,  336,  336,  336,  336,  340,  345,  350,  366,  369,  376,  379,  385,  386,  387,
    388,  389,  390,  391,  392,  393,  394,  395,  396,  399,  402,  405,  408,  411,  414,  417,
    420,  423,  426,  427,  428,  429,  438,  438,  438,  438,  442,  448,  451,  458,  461,  467,
    471,  471,  471,  471,  475,  483,  486,  489,  492,  495,  498,  507,  510,  513,  516,  519,
    522,  525,  528,  531,  534,  537,  540,  543,  546,  549,  552,  560,  563,  566,  569,  572,
    575,  578,  581,  584,  587,  590,  593,  596,  599,  602,  605,  608,  611,  614,  617,  620,
    623,  626,  629,  632,  635,  638,  641,  644,  647,  650,  653,  656,  659,  662,  665,  668,
    671,  674,  677,  680,  683,  686,  689,  692,  695,  698,  701,  704,  707,  710,  713,  716,
    719,  722,  725,  728,  731,  734,  737,  740,  747,  752,  760,  769,  775,  781,  787,  793,
    799,  805,  811,  817,  823,  829,  835,  841,  847,  850,  856,  859,  865,  868,  874,  877,
    883,  886,  892,  893,  894,  895,  896,  897,  898,  899,  900,  901,  902,  903,  904,  905,
    906,  907,  908,  909,  910,  911,  912,  919,  920,  927,  927,  931,  936,  936,  936,  936,
    936,  936,  937,  937,  943,  951,  957,  960,  967,  974,  974,  974,  974,  978,  984,  984,
    984,  984,  984,  984,  984,  984,  984,  984,  984,  984,  984,  985,  985,  985,  985,  989,
    996,  1002, 1007, 1012, 1018, 1023, 1028, 1033, 1039, 1044, 1050, 1059, 1065, 1071, 1076, 1082,
    1088, 1088, 1088, 1092, 1099, 1106, 1113, 1113, 1113, 1113, 1113, 1113, 1113, 1114, 1114, 1114,
    1114, 1114, 1114, 1114, 1114, 1115, 1115, 1115, 1115, 1115, 1115, 1115, 1119, 1129, 1132, 1138,
    1141, 1147, 1156, 1165, 1168, 1171, 1177, 1188, 1199, 1202, 1208, 1216, 1224, 1232, 1235, 1240,
    1249, 1255, 1261, 1267, 1277, 1287, 1294, 1301, 1308, 1316, 1324, 1332, 1340, 1346, 1352, 1352,
    1356, 1363, 1370, 1370, 1374, 1374, 1378, 1384, 1385, 1392, 1398, 1401, 1408, 1415, 1416, 1417,
    1418, 1419, 1422, 1422, 1422, 1422, 1422, 1422, 1422, 1424, 1429, 1434, 1439, 1444, 1449, 1454,
    1460, 1461, 1462, 1463, 1464, 1465, 1466, 1467, 1468, 1469, 1474, 1477, 1484, 1487, 1493, 1503,
    1508, 1513, 1518, 1523, 1528, 1533, 1538, 1543};

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
#line 5502 "pipeline_parser_gen.cpp"

#line 1547 "pipeline_grammar.yy"
