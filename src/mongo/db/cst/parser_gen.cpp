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


#include "parser_gen.hpp"


// Unqualified %code blocks.
#line 82 "grammar.yy"

#include <boost/algorithm/string.hpp>
#include <iterator>
#include <utility>

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void ParserGen::error(const ParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
}
}  // namespace mongo

// Default location for actions, called each time a rule is matched but before the action is
// run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
// mongo.
#define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)

#line 73 "parser_gen.cpp"


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

#line 57 "grammar.yy"
namespace mongo {
#line 166 "parser_gen.cpp"

/// Build a parser object.
ParserGen::ParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      lexer(lexer_yyarg),
      cst(cst_yyarg) {
}

ParserGen::~ParserGen() {}

ParserGen::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------------.
| symbol kinds.  |
`---------------*/


// by_state.
ParserGen::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

ParserGen::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void ParserGen::by_state::clear() YY_NOEXCEPT {
    state = empty_state;
}

void ParserGen::by_state::move(by_state& that) {
    state = that.state;
    that.clear();
}

ParserGen::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

ParserGen::symbol_kind_type ParserGen::by_state::kind() const YY_NOEXCEPT {
    if (state == empty_state)
        return symbol_kind::S_YYEMPTY;
    else
        return YY_CAST(symbol_kind_type, yystos_[+state]);
}

ParserGen::stack_symbol_type::stack_symbol_type() {}

ParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 134:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 141:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 143:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 140:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 139:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 142:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 137:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 147:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 136:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 148:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 150:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 149:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 138:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 135:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 146:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 144:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 145:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 129:  // "fieldname containing dotted path"
            value.YY_MOVE_OR_COPY<std::vector<std::string>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

#if 201103L <= YY_CPLUSPLUS
    // that is emptied.
    that.state = empty_state;
#endif
}

ParserGen::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
    : super_type(s, YY_MOVE(that.location)) {
    switch (that.kind()) {
        case 134:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 141:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 143:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 140:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 139:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 142:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 137:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 147:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 136:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 148:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 150:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 149:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 138:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 135:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 146:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 144:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 145:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 129:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.kind_ = symbol_kind::S_YYEMPTY;
}

#if YY_CPLUSPLUS < 201103L
ParserGen::stack_symbol_type& ParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 134:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 141:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 143:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 140:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 139:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 142:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 137:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 147:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 136:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 148:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 150:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 149:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 138:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 135:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 146:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 144:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 145:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.copy<std::string>(that.value);
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(that.value);
            break;

        case 129:  // "fieldname containing dotted path"
            value.copy<std::vector<std::string>>(that.value);
            break;

        default:
            break;
    }

    location = that.location;
    return *this;
}

ParserGen::stack_symbol_type& ParserGen::stack_symbol_type::operator=(stack_symbol_type& that) {
    state = that.state;
    switch (that.kind()) {
        case 134:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 141:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 143:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 140:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 139:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 142:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 172:  // dbPointer
        case 173:  // javascript
        case 174:  // symbol
        case 175:  // javascriptWScope
        case 176:  // int
        case 177:  // timestamp
        case 178:  // long
        case 179:  // double
        case 180:  // decimal
        case 181:  // minKey
        case 182:  // maxKey
        case 183:  // value
        case 184:  // string
        case 185:  // aggregationFieldPath
        case 186:  // binary
        case 187:  // undefined
        case 188:  // objectId
        case 189:  // bool
        case 190:  // date
        case 191:  // null
        case 192:  // regex
        case 193:  // simpleValue
        case 194:  // compoundValue
        case 195:  // valueArray
        case 196:  // valueObject
        case 197:  // valueFields
        case 198:  // variable
        case 199:  // pipeline
        case 200:  // stageList
        case 201:  // stage
        case 202:  // inhibitOptimization
        case 203:  // unionWith
        case 204:  // skip
        case 205:  // limit
        case 206:  // project
        case 207:  // sample
        case 208:  // projectFields
        case 209:  // projectionObjectFields
        case 210:  // topLevelProjection
        case 211:  // projection
        case 212:  // projectionObject
        case 213:  // num
        case 214:  // expression
        case 215:  // compoundNonObjectExpression
        case 216:  // exprFixedTwoArg
        case 217:  // exprFixedThreeArg
        case 218:  // arrayManipulation
        case 219:  // slice
        case 220:  // expressionArray
        case 221:  // expressionObject
        case 222:  // expressionFields
        case 223:  // maths
        case 224:  // meta
        case 225:  // add
        case 226:  // atan2
        case 227:  // boolExprs
        case 228:  // and
        case 229:  // or
        case 230:  // not
        case 231:  // literalEscapes
        case 232:  // const
        case 233:  // literal
        case 234:  // stringExps
        case 235:  // concat
        case 236:  // dateFromString
        case 237:  // dateToString
        case 238:  // indexOfBytes
        case 239:  // indexOfCP
        case 240:  // ltrim
        case 241:  // regexFind
        case 242:  // regexFindAll
        case 243:  // regexMatch
        case 244:  // regexArgs
        case 245:  // replaceOne
        case 246:  // replaceAll
        case 247:  // rtrim
        case 248:  // split
        case 249:  // strLenBytes
        case 250:  // strLenCP
        case 251:  // strcasecmp
        case 252:  // substr
        case 253:  // substrBytes
        case 254:  // substrCP
        case 255:  // toLower
        case 256:  // toUpper
        case 257:  // trim
        case 258:  // compExprs
        case 259:  // cmp
        case 260:  // eq
        case 261:  // gt
        case 262:  // gte
        case 263:  // lt
        case 264:  // lte
        case 265:  // ne
        case 266:  // typeExpression
        case 267:  // convert
        case 268:  // toBool
        case 269:  // toDate
        case 270:  // toDecimal
        case 271:  // toDouble
        case 272:  // toInt
        case 273:  // toLong
        case 274:  // toObjectId
        case 275:  // toString
        case 276:  // type
        case 277:  // abs
        case 278:  // ceil
        case 279:  // divide
        case 280:  // exponent
        case 281:  // floor
        case 282:  // ln
        case 283:  // log
        case 284:  // logten
        case 285:  // mod
        case 286:  // multiply
        case 287:  // pow
        case 288:  // round
        case 289:  // sqrt
        case 290:  // subtract
        case 291:  // trunc
        case 301:  // setExpression
        case 302:  // allElementsTrue
        case 303:  // anyElementTrue
        case 304:  // setDifference
        case 305:  // setEquals
        case 306:  // setIntersection
        case 307:  // setIsSubset
        case 308:  // setUnion
        case 309:  // match
        case 310:  // predicates
        case 311:  // compoundMatchExprs
        case 312:  // predValue
        case 313:  // additionalExprs
        case 319:  // sortSpecs
        case 320:  // specList
        case 321:  // metaSort
        case 322:  // oneOrNegOne
        case 323:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 155:  // aggregationProjectionFieldname
        case 156:  // projectionFieldname
        case 157:  // expressionFieldname
        case 158:  // stageAsUserFieldname
        case 159:  // argAsUserFieldname
        case 160:  // argAsProjectionPath
        case 161:  // aggExprAsUserFieldname
        case 162:  // invariableUserFieldname
        case 163:  // idAsUserFieldname
        case 164:  // idAsProjectionPath
        case 165:  // valueFieldname
        case 166:  // predFieldname
        case 318:  // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case 137:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 147:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 136:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 148:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 150:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 149:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 138:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 135:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 146:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 144:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 145:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 167:  // projectField
        case 168:  // projectionObjectField
        case 169:  // expressionField
        case 170:  // valueField
        case 292:  // onErrorArg
        case 293:  // onNullArg
        case 294:  // formatArg
        case 295:  // timezoneArg
        case 296:  // charsArg
        case 297:  // optionsArg
        case 314:  // predicate
        case 315:  // logicalExpr
        case 316:  // operatorExpression
        case 317:  // notExpr
        case 324:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 128:  // "fieldname"
        case 130:  // "$-prefixed fieldname"
        case 131:  // "string"
        case 132:  // "$-prefixed string"
        case 133:  // "$$-prefixed string"
        case 171:  // arg
            value.move<std::string>(that.value);
            break;

        case 298:  // expressions
        case 299:  // values
        case 300:  // exprZeroToTwo
            value.move<std::vector<CNode>>(that.value);
            break;

        case 129:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(that.value);
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
void ParserGen::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
    if (yymsg)
        YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void ParserGen::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
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

void ParserGen::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
    if (m)
        YY_SYMBOL_PRINT(m, sym);
    yystack_.push(YY_MOVE(sym));
}

void ParserGen::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
    yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
    stack_symbol_type ss(s, sym);
    yypush_(m, ss);
#endif
}

void ParserGen::yypop_(int n) {
    yystack_.pop(n);
}

#if YYDEBUG
std::ostream& ParserGen::debug_stream() const {
    return *yycdebug_;
}

void ParserGen::set_debug_stream(std::ostream& o) {
    yycdebug_ = &o;
}


ParserGen::debug_level_type ParserGen::debug_level() const {
    return yydebug_;
}

void ParserGen::set_debug_level(debug_level_type l) {
    yydebug_ = l;
}
#endif  // YYDEBUG

ParserGen::state_type ParserGen::yy_lr_goto_state_(state_type yystate, int yysym) {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - YYNTOKENS];
}

bool ParserGen::yy_pact_value_is_default_(int yyvalue) {
    return yyvalue == yypact_ninf_;
}

bool ParserGen::yy_table_value_is_error_(int yyvalue) {
    return yyvalue == yytable_ninf_;
}

int ParserGen::operator()() {
    return parse();
}

int ParserGen::parse() {
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
                case 134:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 141:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 143:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 140:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 139:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 142:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 172:  // dbPointer
                case 173:  // javascript
                case 174:  // symbol
                case 175:  // javascriptWScope
                case 176:  // int
                case 177:  // timestamp
                case 178:  // long
                case 179:  // double
                case 180:  // decimal
                case 181:  // minKey
                case 182:  // maxKey
                case 183:  // value
                case 184:  // string
                case 185:  // aggregationFieldPath
                case 186:  // binary
                case 187:  // undefined
                case 188:  // objectId
                case 189:  // bool
                case 190:  // date
                case 191:  // null
                case 192:  // regex
                case 193:  // simpleValue
                case 194:  // compoundValue
                case 195:  // valueArray
                case 196:  // valueObject
                case 197:  // valueFields
                case 198:  // variable
                case 199:  // pipeline
                case 200:  // stageList
                case 201:  // stage
                case 202:  // inhibitOptimization
                case 203:  // unionWith
                case 204:  // skip
                case 205:  // limit
                case 206:  // project
                case 207:  // sample
                case 208:  // projectFields
                case 209:  // projectionObjectFields
                case 210:  // topLevelProjection
                case 211:  // projection
                case 212:  // projectionObject
                case 213:  // num
                case 214:  // expression
                case 215:  // compoundNonObjectExpression
                case 216:  // exprFixedTwoArg
                case 217:  // exprFixedThreeArg
                case 218:  // arrayManipulation
                case 219:  // slice
                case 220:  // expressionArray
                case 221:  // expressionObject
                case 222:  // expressionFields
                case 223:  // maths
                case 224:  // meta
                case 225:  // add
                case 226:  // atan2
                case 227:  // boolExprs
                case 228:  // and
                case 229:  // or
                case 230:  // not
                case 231:  // literalEscapes
                case 232:  // const
                case 233:  // literal
                case 234:  // stringExps
                case 235:  // concat
                case 236:  // dateFromString
                case 237:  // dateToString
                case 238:  // indexOfBytes
                case 239:  // indexOfCP
                case 240:  // ltrim
                case 241:  // regexFind
                case 242:  // regexFindAll
                case 243:  // regexMatch
                case 244:  // regexArgs
                case 245:  // replaceOne
                case 246:  // replaceAll
                case 247:  // rtrim
                case 248:  // split
                case 249:  // strLenBytes
                case 250:  // strLenCP
                case 251:  // strcasecmp
                case 252:  // substr
                case 253:  // substrBytes
                case 254:  // substrCP
                case 255:  // toLower
                case 256:  // toUpper
                case 257:  // trim
                case 258:  // compExprs
                case 259:  // cmp
                case 260:  // eq
                case 261:  // gt
                case 262:  // gte
                case 263:  // lt
                case 264:  // lte
                case 265:  // ne
                case 266:  // typeExpression
                case 267:  // convert
                case 268:  // toBool
                case 269:  // toDate
                case 270:  // toDecimal
                case 271:  // toDouble
                case 272:  // toInt
                case 273:  // toLong
                case 274:  // toObjectId
                case 275:  // toString
                case 276:  // type
                case 277:  // abs
                case 278:  // ceil
                case 279:  // divide
                case 280:  // exponent
                case 281:  // floor
                case 282:  // ln
                case 283:  // log
                case 284:  // logten
                case 285:  // mod
                case 286:  // multiply
                case 287:  // pow
                case 288:  // round
                case 289:  // sqrt
                case 290:  // subtract
                case 291:  // trunc
                case 301:  // setExpression
                case 302:  // allElementsTrue
                case 303:  // anyElementTrue
                case 304:  // setDifference
                case 305:  // setEquals
                case 306:  // setIntersection
                case 307:  // setIsSubset
                case 308:  // setUnion
                case 309:  // match
                case 310:  // predicates
                case 311:  // compoundMatchExprs
                case 312:  // predValue
                case 313:  // additionalExprs
                case 319:  // sortSpecs
                case 320:  // specList
                case 321:  // metaSort
                case 322:  // oneOrNegOne
                case 323:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 155:  // aggregationProjectionFieldname
                case 156:  // projectionFieldname
                case 157:  // expressionFieldname
                case 158:  // stageAsUserFieldname
                case 159:  // argAsUserFieldname
                case 160:  // argAsProjectionPath
                case 161:  // aggExprAsUserFieldname
                case 162:  // invariableUserFieldname
                case 163:  // idAsUserFieldname
                case 164:  // idAsProjectionPath
                case 165:  // valueFieldname
                case 166:  // predFieldname
                case 318:  // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 137:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 147:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 136:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 148:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 150:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 149:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 138:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 135:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 146:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 144:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 145:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 167:  // projectField
                case 168:  // projectionObjectField
                case 169:  // expressionField
                case 170:  // valueField
                case 292:  // onErrorArg
                case 293:  // onNullArg
                case 294:  // formatArg
                case 295:  // timezoneArg
                case 296:  // charsArg
                case 297:  // optionsArg
                case 314:  // predicate
                case 315:  // logicalExpr
                case 316:  // operatorExpression
                case 317:  // notExpr
                case 324:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 128:  // "fieldname"
                case 130:  // "$-prefixed fieldname"
                case 131:  // "string"
                case 132:  // "$-prefixed string"
                case 133:  // "$$-prefixed string"
                case 171:  // arg
                    yylhs.value.emplace<std::string>();
                    break;

                case 298:  // expressions
                case 299:  // values
                case 300:  // exprZeroToTwo
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                case 129:  // "fieldname containing dotted path"
                    yylhs.value.emplace<std::vector<std::string>>();
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
#line 333 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1873 "parser_gen.cpp"
                    break;

                    case 3:
#line 336 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1881 "parser_gen.cpp"
                    break;

                    case 4:
#line 339 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1889 "parser_gen.cpp"
                    break;

                    case 5:
#line 342 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1897 "parser_gen.cpp"
                    break;

                    case 6:
#line 345 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1905 "parser_gen.cpp"
                    break;

                    case 7:
#line 352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1913 "parser_gen.cpp"
                    break;

                    case 8:
#line 358 "grammar.yy"
                    {
                    }
#line 1919 "parser_gen.cpp"
                    break;

                    case 9:
#line 359 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1927 "parser_gen.cpp"
                    break;

                    case 10:
#line 367 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1933 "parser_gen.cpp"
                    break;

                    case 12:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1939 "parser_gen.cpp"
                    break;

                    case 13:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1945 "parser_gen.cpp"
                    break;

                    case 14:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1951 "parser_gen.cpp"
                    break;

                    case 15:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1957 "parser_gen.cpp"
                    break;

                    case 16:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1963 "parser_gen.cpp"
                    break;

                    case 17:
#line 370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1969 "parser_gen.cpp"
                    break;

                    case 18:
#line 373 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1981 "parser_gen.cpp"
                    break;

                    case 19:
#line 383 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1989 "parser_gen.cpp"
                    break;

                    case 20:
#line 389 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2002 "parser_gen.cpp"
                    break;

                    case 21:
#line 399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2008 "parser_gen.cpp"
                    break;

                    case 22:
#line 399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2014 "parser_gen.cpp"
                    break;

                    case 23:
#line 399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2020 "parser_gen.cpp"
                    break;

                    case 24:
#line 399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2026 "parser_gen.cpp"
                    break;

                    case 25:
#line 403 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2034 "parser_gen.cpp"
                    break;

                    case 26:
#line 408 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2042 "parser_gen.cpp"
                    break;

                    case 27:
#line 413 "grammar.yy"
                    {
                        auto&& fields = YY_MOVE(yystack_[1].value.as<CNode>());
                        if (auto status =
                                c_node_validation::validateNoConflictingPathsInProjectFields(
                                    fields);
                            !status.isOK())
                            error(yystack_[3].location, status.reason());
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
#line 2063 "parser_gen.cpp"
                    break;

                    case 28:
#line 432 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2071 "parser_gen.cpp"
                    break;

                    case 29:
#line 435 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2080 "parser_gen.cpp"
                    break;

                    case 30:
#line 442 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2088 "parser_gen.cpp"
                    break;

                    case 31:
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2096 "parser_gen.cpp"
                    break;

                    case 32:
#line 451 "grammar.yy"
                    {
                        auto projection = YY_MOVE(yystack_[0].value.as<CNode>());
                        yylhs.value.as<CNode>() =
                            stdx::holds_alternative<CNode::ObjectChildren>(projection.payload) &&
                                stdx::holds_alternative<FieldnamePath>(
                                    projection.objectChildren()[0].first)
                            ? c_node_disambiguation::disambiguateCompoundProjection(
                                  std::move(projection))
                            : std::move(projection);
                        if (stdx::holds_alternative<CompoundInconsistentKey>(
                                yylhs.value.as<CNode>().payload))
                            // TODO SERVER-50498: error() instead of uasserting
                            uasserted(ErrorCodes::FailedToParse,
                                      "object project field cannot contain both "
                                      "inclusion and exclusion indicators");
                    }
#line 2112 "parser_gen.cpp"
                    break;

                    case 33:
#line 465 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2118 "parser_gen.cpp"
                    break;

                    case 34:
#line 466 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2124 "parser_gen.cpp"
                    break;

                    case 35:
#line 467 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2130 "parser_gen.cpp"
                    break;

                    case 36:
#line 468 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2136 "parser_gen.cpp"
                    break;

                    case 37:
#line 469 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2142 "parser_gen.cpp"
                    break;

                    case 38:
#line 470 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2148 "parser_gen.cpp"
                    break;

                    case 39:
#line 471 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2154 "parser_gen.cpp"
                    break;

                    case 40:
#line 472 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2160 "parser_gen.cpp"
                    break;

                    case 41:
#line 473 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2166 "parser_gen.cpp"
                    break;

                    case 42:
#line 474 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2172 "parser_gen.cpp"
                    break;

                    case 43:
#line 475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2178 "parser_gen.cpp"
                    break;

                    case 44:
#line 476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2186 "parser_gen.cpp"
                    break;

                    case 45:
#line 479 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2194 "parser_gen.cpp"
                    break;

                    case 46:
#line 482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2202 "parser_gen.cpp"
                    break;

                    case 47:
#line 485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2210 "parser_gen.cpp"
                    break;

                    case 48:
#line 488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2218 "parser_gen.cpp"
                    break;

                    case 49:
#line 491 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2226 "parser_gen.cpp"
                    break;

                    case 50:
#line 494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2234 "parser_gen.cpp"
                    break;

                    case 51:
#line 497 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2242 "parser_gen.cpp"
                    break;

                    case 52:
#line 500 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2250 "parser_gen.cpp"
                    break;

                    case 53:
#line 503 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2258 "parser_gen.cpp"
                    break;

                    case 54:
#line 506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2266 "parser_gen.cpp"
                    break;

                    case 55:
#line 509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2274 "parser_gen.cpp"
                    break;

                    case 56:
#line 512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2282 "parser_gen.cpp"
                    break;

                    case 57:
#line 515 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2290 "parser_gen.cpp"
                    break;

                    case 58:
#line 518 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2298 "parser_gen.cpp"
                    break;

                    case 59:
#line 521 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2306 "parser_gen.cpp"
                    break;

                    case 60:
#line 524 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2314 "parser_gen.cpp"
                    break;

                    case 61:
#line 527 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2322 "parser_gen.cpp"
                    break;

                    case 62:
#line 530 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2328 "parser_gen.cpp"
                    break;

                    case 63:
#line 531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2334 "parser_gen.cpp"
                    break;

                    case 64:
#line 532 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2340 "parser_gen.cpp"
                    break;

                    case 65:
#line 533 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2346 "parser_gen.cpp"
                    break;

                    case 66:
#line 534 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2352 "parser_gen.cpp"
                    break;

                    case 67:
#line 539 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2362 "parser_gen.cpp"
                    break;

                    case 68:
#line 547 "grammar.yy"
                    {
                        auto components =
                            make_vector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK()) {
                            if (positional.getValue() == c_node_validation::IsPositional::yes)
                                yylhs.value.as<CNode::Fieldname>() =
                                    PositionalProjectionPath{std::move(components)};
                            else
                                yylhs.value.as<CNode::Fieldname>() =
                                    ProjectionPath{std::move(components)};
                        } else {
                            error(yystack_[0].location, positional.getStatus().reason());
                        }
                    }
#line 2380 "parser_gen.cpp"
                    break;

                    case 69:
#line 560 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2386 "parser_gen.cpp"
                    break;

                    case 70:
#line 561 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK()) {
                            if (positional.getValue() == c_node_validation::IsPositional::yes)
                                yylhs.value.as<CNode::Fieldname>() =
                                    PositionalProjectionPath{std::move(components)};
                            else
                                yylhs.value.as<CNode::Fieldname>() =
                                    ProjectionPath{std::move(components)};
                        } else {
                            error(yystack_[0].location, positional.getStatus().reason());
                        }
                    }
#line 2404 "parser_gen.cpp"
                    break;

                    case 71:
#line 578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2412 "parser_gen.cpp"
                    break;

                    case 72:
#line 585 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2421 "parser_gen.cpp"
                    break;

                    case 73:
#line 589 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2430 "parser_gen.cpp"
                    break;

                    case 74:
#line 597 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2438 "parser_gen.cpp"
                    break;

                    case 75:
#line 600 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2446 "parser_gen.cpp"
                    break;

                    case 76:
#line 606 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2454 "parser_gen.cpp"
                    break;

                    case 77:
#line 612 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2462 "parser_gen.cpp"
                    break;

                    case 78:
#line 615 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2471 "parser_gen.cpp"
                    break;

                    case 79:
#line 621 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2479 "parser_gen.cpp"
                    break;

                    case 80:
#line 624 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2487 "parser_gen.cpp"
                    break;

                    case 81:
#line 633 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2493 "parser_gen.cpp"
                    break;

                    case 82:
#line 634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2501 "parser_gen.cpp"
                    break;

                    case 83:
#line 640 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2509 "parser_gen.cpp"
                    break;

                    case 84:
#line 643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2518 "parser_gen.cpp"
                    break;

                    case 85:
#line 650 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2524 "parser_gen.cpp"
                    break;

                    case 86:
#line 653 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2532 "parser_gen.cpp"
                    break;

                    case 87:
#line 657 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[1].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2543 "parser_gen.cpp"
                    break;

                    case 88:
#line 666 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[1].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2553 "parser_gen.cpp"
                    break;

                    case 89:
#line 674 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2559 "parser_gen.cpp"
                    break;

                    case 90:
#line 675 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2565 "parser_gen.cpp"
                    break;

                    case 91:
#line 676 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2571 "parser_gen.cpp"
                    break;

                    case 92:
#line 679 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2579 "parser_gen.cpp"
                    break;

                    case 93:
#line 682 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2588 "parser_gen.cpp"
                    break;

                    case 94:
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2594 "parser_gen.cpp"
                    break;

                    case 95:
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2600 "parser_gen.cpp"
                    break;

                    case 96:
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2606 "parser_gen.cpp"
                    break;

                    case 97:
#line 692 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2614 "parser_gen.cpp"
                    break;

                    case 98:
#line 700 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2622 "parser_gen.cpp"
                    break;

                    case 99:
#line 703 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2630 "parser_gen.cpp"
                    break;

                    case 100:
#line 706 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2638 "parser_gen.cpp"
                    break;

                    case 101:
#line 709 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2646 "parser_gen.cpp"
                    break;

                    case 102:
#line 712 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2654 "parser_gen.cpp"
                    break;

                    case 103:
#line 715 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2662 "parser_gen.cpp"
                    break;

                    case 104:
#line 721 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2670 "parser_gen.cpp"
                    break;

                    case 105:
#line 727 "grammar.yy"
                    {
                        auto components =
                            make_vector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK()) {
                            if (positional.getValue() == c_node_validation::IsPositional::yes)
                                yylhs.value.as<CNode::Fieldname>() =
                                    PositionalProjectionPath{std::move(components)};
                            else
                                yylhs.value.as<CNode::Fieldname>() =
                                    ProjectionPath{std::move(components)};
                        } else {
                            error(yystack_[0].location, positional.getStatus().reason());
                        }
                    }
#line 2688 "parser_gen.cpp"
                    break;

                    case 106:
#line 746 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 2696 "parser_gen.cpp"
                    break;

                    case 107:
#line 749 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 2704 "parser_gen.cpp"
                    break;

                    case 108:
#line 752 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 2712 "parser_gen.cpp"
                    break;

                    case 109:
#line 755 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 2720 "parser_gen.cpp"
                    break;

                    case 110:
#line 758 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 2728 "parser_gen.cpp"
                    break;

                    case 111:
#line 761 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 2736 "parser_gen.cpp"
                    break;

                    case 112:
#line 764 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 2744 "parser_gen.cpp"
                    break;

                    case 113:
#line 767 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 2752 "parser_gen.cpp"
                    break;

                    case 114:
#line 770 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 2760 "parser_gen.cpp"
                    break;

                    case 115:
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 2768 "parser_gen.cpp"
                    break;

                    case 116:
#line 776 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 2776 "parser_gen.cpp"
                    break;

                    case 117:
#line 779 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 2784 "parser_gen.cpp"
                    break;

                    case 118:
#line 782 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 2792 "parser_gen.cpp"
                    break;

                    case 119:
#line 785 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 2800 "parser_gen.cpp"
                    break;

                    case 120:
#line 788 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 2808 "parser_gen.cpp"
                    break;

                    case 121:
#line 791 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 2816 "parser_gen.cpp"
                    break;

                    case 122:
#line 794 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"filter"};
                    }
#line 2824 "parser_gen.cpp"
                    break;

                    case 123:
#line 797 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"q"};
                    }
#line 2832 "parser_gen.cpp"
                    break;

                    case 124:
#line 805 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2840 "parser_gen.cpp"
                    break;

                    case 125:
#line 808 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2848 "parser_gen.cpp"
                    break;

                    case 126:
#line 811 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2856 "parser_gen.cpp"
                    break;

                    case 127:
#line 814 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2864 "parser_gen.cpp"
                    break;

                    case 128:
#line 817 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2872 "parser_gen.cpp"
                    break;

                    case 129:
#line 820 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2880 "parser_gen.cpp"
                    break;

                    case 130:
#line 823 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2888 "parser_gen.cpp"
                    break;

                    case 131:
#line 826 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2896 "parser_gen.cpp"
                    break;

                    case 132:
#line 829 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2904 "parser_gen.cpp"
                    break;

                    case 133:
#line 832 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2912 "parser_gen.cpp"
                    break;

                    case 134:
#line 835 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2920 "parser_gen.cpp"
                    break;

                    case 135:
#line 838 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2928 "parser_gen.cpp"
                    break;

                    case 136:
#line 841 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2936 "parser_gen.cpp"
                    break;

                    case 137:
#line 844 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2944 "parser_gen.cpp"
                    break;

                    case 138:
#line 847 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2952 "parser_gen.cpp"
                    break;

                    case 139:
#line 850 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2960 "parser_gen.cpp"
                    break;

                    case 140:
#line 853 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2968 "parser_gen.cpp"
                    break;

                    case 141:
#line 856 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2976 "parser_gen.cpp"
                    break;

                    case 142:
#line 859 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2984 "parser_gen.cpp"
                    break;

                    case 143:
#line 862 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2992 "parser_gen.cpp"
                    break;

                    case 144:
#line 865 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3000 "parser_gen.cpp"
                    break;

                    case 145:
#line 868 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3008 "parser_gen.cpp"
                    break;

                    case 146:
#line 871 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3016 "parser_gen.cpp"
                    break;

                    case 147:
#line 874 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3024 "parser_gen.cpp"
                    break;

                    case 148:
#line 877 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3032 "parser_gen.cpp"
                    break;

                    case 149:
#line 880 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3040 "parser_gen.cpp"
                    break;

                    case 150:
#line 883 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3048 "parser_gen.cpp"
                    break;

                    case 151:
#line 886 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3056 "parser_gen.cpp"
                    break;

                    case 152:
#line 889 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3064 "parser_gen.cpp"
                    break;

                    case 153:
#line 892 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3072 "parser_gen.cpp"
                    break;

                    case 154:
#line 895 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3080 "parser_gen.cpp"
                    break;

                    case 155:
#line 898 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3088 "parser_gen.cpp"
                    break;

                    case 156:
#line 901 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3096 "parser_gen.cpp"
                    break;

                    case 157:
#line 904 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3104 "parser_gen.cpp"
                    break;

                    case 158:
#line 907 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3112 "parser_gen.cpp"
                    break;

                    case 159:
#line 910 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3120 "parser_gen.cpp"
                    break;

                    case 160:
#line 913 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3128 "parser_gen.cpp"
                    break;

                    case 161:
#line 916 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3136 "parser_gen.cpp"
                    break;

                    case 162:
#line 919 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3144 "parser_gen.cpp"
                    break;

                    case 163:
#line 922 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3152 "parser_gen.cpp"
                    break;

                    case 164:
#line 925 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3160 "parser_gen.cpp"
                    break;

                    case 165:
#line 928 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3168 "parser_gen.cpp"
                    break;

                    case 166:
#line 931 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3176 "parser_gen.cpp"
                    break;

                    case 167:
#line 934 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3184 "parser_gen.cpp"
                    break;

                    case 168:
#line 937 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3192 "parser_gen.cpp"
                    break;

                    case 169:
#line 940 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3200 "parser_gen.cpp"
                    break;

                    case 170:
#line 943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3208 "parser_gen.cpp"
                    break;

                    case 171:
#line 946 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3216 "parser_gen.cpp"
                    break;

                    case 172:
#line 949 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3224 "parser_gen.cpp"
                    break;

                    case 173:
#line 952 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3232 "parser_gen.cpp"
                    break;

                    case 174:
#line 955 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3240 "parser_gen.cpp"
                    break;

                    case 175:
#line 958 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3248 "parser_gen.cpp"
                    break;

                    case 176:
#line 961 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3256 "parser_gen.cpp"
                    break;

                    case 177:
#line 964 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3264 "parser_gen.cpp"
                    break;

                    case 178:
#line 967 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3272 "parser_gen.cpp"
                    break;

                    case 179:
#line 970 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3280 "parser_gen.cpp"
                    break;

                    case 180:
#line 973 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3288 "parser_gen.cpp"
                    break;

                    case 181:
#line 976 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3296 "parser_gen.cpp"
                    break;

                    case 182:
#line 979 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3304 "parser_gen.cpp"
                    break;

                    case 183:
#line 982 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3312 "parser_gen.cpp"
                    break;

                    case 184:
#line 985 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3320 "parser_gen.cpp"
                    break;

                    case 185:
#line 988 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3328 "parser_gen.cpp"
                    break;

                    case 186:
#line 991 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3336 "parser_gen.cpp"
                    break;

                    case 187:
#line 994 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3344 "parser_gen.cpp"
                    break;

                    case 188:
#line 997 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3352 "parser_gen.cpp"
                    break;

                    case 189:
#line 1000 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3360 "parser_gen.cpp"
                    break;

                    case 190:
#line 1003 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3368 "parser_gen.cpp"
                    break;

                    case 191:
#line 1006 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3376 "parser_gen.cpp"
                    break;

                    case 192:
#line 1009 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3384 "parser_gen.cpp"
                    break;

                    case 193:
#line 1012 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3392 "parser_gen.cpp"
                    break;

                    case 194:
#line 1019 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3400 "parser_gen.cpp"
                    break;

                    case 195:
#line 1024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 3408 "parser_gen.cpp"
                    break;

                    case 196:
#line 1027 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 3416 "parser_gen.cpp"
                    break;

                    case 197:
#line 1030 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 3424 "parser_gen.cpp"
                    break;

                    case 198:
#line 1033 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3432 "parser_gen.cpp"
                    break;

                    case 199:
#line 1036 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 3440 "parser_gen.cpp"
                    break;

                    case 200:
#line 1039 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 3448 "parser_gen.cpp"
                    break;

                    case 201:
#line 1042 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 3456 "parser_gen.cpp"
                    break;

                    case 202:
#line 1045 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 3464 "parser_gen.cpp"
                    break;

                    case 203:
#line 1048 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3472 "parser_gen.cpp"
                    break;

                    case 204:
#line 1054 "grammar.yy"
                    {
                        auto str = YY_MOVE(yystack_[0].value.as<std::string>());
                        auto components = std::vector<std::string>{};
                        auto withoutDollar = std::pair{std::next(str.begin()), str.end()};
                        boost::split(components, withoutDollar, [](auto&& c) { return c == '.'; });
                        if (auto status = c_node_validation::validateAggregationPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode>() = CNode{AggregationPath{std::move(components)}};
                    }
#line 3488 "parser_gen.cpp"
                    break;

                    case 205:
#line 1068 "grammar.yy"
                    {
                        auto str = YY_MOVE(yystack_[0].value.as<std::string>());
                        auto components = std::vector<std::string>{};
                        auto withoutDollars =
                            std::pair{std::next(std::next(str.begin())), str.end()};
                        boost::split(components, withoutDollars, [](auto&& c) { return c == '.'; });
                        if (auto status =
                                c_node_validation::validateVariableNameAndPathSuffix(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode>() =
                            CNode{AggregationVariablePath{std::move(components)}};
                    }
#line 3504 "parser_gen.cpp"
                    break;

                    case 206:
#line 1082 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3512 "parser_gen.cpp"
                    break;

                    case 207:
#line 1088 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3520 "parser_gen.cpp"
                    break;

                    case 208:
#line 1094 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3528 "parser_gen.cpp"
                    break;

                    case 209:
#line 1100 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3536 "parser_gen.cpp"
                    break;

                    case 210:
#line 1106 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3544 "parser_gen.cpp"
                    break;

                    case 211:
#line 1112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3552 "parser_gen.cpp"
                    break;

                    case 212:
#line 1118 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3560 "parser_gen.cpp"
                    break;

                    case 213:
#line 1124 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3568 "parser_gen.cpp"
                    break;

                    case 214:
#line 1130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3576 "parser_gen.cpp"
                    break;

                    case 215:
#line 1136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3584 "parser_gen.cpp"
                    break;

                    case 216:
#line 1142 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3592 "parser_gen.cpp"
                    break;

                    case 217:
#line 1148 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3600 "parser_gen.cpp"
                    break;

                    case 218:
#line 1154 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3608 "parser_gen.cpp"
                    break;

                    case 219:
#line 1160 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3616 "parser_gen.cpp"
                    break;

                    case 220:
#line 1163 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3624 "parser_gen.cpp"
                    break;

                    case 221:
#line 1166 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3632 "parser_gen.cpp"
                    break;

                    case 222:
#line 1169 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3640 "parser_gen.cpp"
                    break;

                    case 223:
#line 1175 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3648 "parser_gen.cpp"
                    break;

                    case 224:
#line 1178 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3656 "parser_gen.cpp"
                    break;

                    case 225:
#line 1181 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3664 "parser_gen.cpp"
                    break;

                    case 226:
#line 1184 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3672 "parser_gen.cpp"
                    break;

                    case 227:
#line 1190 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3680 "parser_gen.cpp"
                    break;

                    case 228:
#line 1193 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3688 "parser_gen.cpp"
                    break;

                    case 229:
#line 1196 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3696 "parser_gen.cpp"
                    break;

                    case 230:
#line 1199 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3704 "parser_gen.cpp"
                    break;

                    case 231:
#line 1205 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3712 "parser_gen.cpp"
                    break;

                    case 232:
#line 1208 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3720 "parser_gen.cpp"
                    break;

                    case 233:
#line 1211 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3728 "parser_gen.cpp"
                    break;

                    case 234:
#line 1214 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3736 "parser_gen.cpp"
                    break;

                    case 235:
#line 1220 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3744 "parser_gen.cpp"
                    break;

                    case 236:
#line 1223 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3752 "parser_gen.cpp"
                    break;

                    case 237:
#line 1229 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3758 "parser_gen.cpp"
                    break;

                    case 238:
#line 1230 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3764 "parser_gen.cpp"
                    break;

                    case 239:
#line 1231 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3770 "parser_gen.cpp"
                    break;

                    case 240:
#line 1232 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3776 "parser_gen.cpp"
                    break;

                    case 241:
#line 1233 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3782 "parser_gen.cpp"
                    break;

                    case 242:
#line 1234 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3788 "parser_gen.cpp"
                    break;

                    case 243:
#line 1235 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3794 "parser_gen.cpp"
                    break;

                    case 244:
#line 1236 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3800 "parser_gen.cpp"
                    break;

                    case 245:
#line 1237 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3806 "parser_gen.cpp"
                    break;

                    case 246:
#line 1238 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3812 "parser_gen.cpp"
                    break;

                    case 247:
#line 1239 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3818 "parser_gen.cpp"
                    break;

                    case 248:
#line 1240 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3824 "parser_gen.cpp"
                    break;

                    case 249:
#line 1241 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3830 "parser_gen.cpp"
                    break;

                    case 250:
#line 1242 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3836 "parser_gen.cpp"
                    break;

                    case 251:
#line 1243 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3842 "parser_gen.cpp"
                    break;

                    case 252:
#line 1244 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3848 "parser_gen.cpp"
                    break;

                    case 253:
#line 1245 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3854 "parser_gen.cpp"
                    break;

                    case 254:
#line 1246 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3860 "parser_gen.cpp"
                    break;

                    case 255:
#line 1247 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3866 "parser_gen.cpp"
                    break;

                    case 256:
#line 1248 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3872 "parser_gen.cpp"
                    break;

                    case 257:
#line 1249 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3878 "parser_gen.cpp"
                    break;

                    case 258:
#line 1256 "grammar.yy"
                    {
                    }
#line 3884 "parser_gen.cpp"
                    break;

                    case 259:
#line 1257 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3893 "parser_gen.cpp"
                    break;

                    case 260:
#line 1264 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3899 "parser_gen.cpp"
                    break;

                    case 261:
#line 1264 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3905 "parser_gen.cpp"
                    break;

                    case 262:
#line 1264 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3911 "parser_gen.cpp"
                    break;

                    case 263:
#line 1269 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3919 "parser_gen.cpp"
                    break;

                    case 264:
#line 1276 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3927 "parser_gen.cpp"
                    break;

                    case 265:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3933 "parser_gen.cpp"
                    break;

                    case 266:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3939 "parser_gen.cpp"
                    break;

                    case 267:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3945 "parser_gen.cpp"
                    break;

                    case 268:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3951 "parser_gen.cpp"
                    break;

                    case 269:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3957 "parser_gen.cpp"
                    break;

                    case 270:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3963 "parser_gen.cpp"
                    break;

                    case 271:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3969 "parser_gen.cpp"
                    break;

                    case 272:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3975 "parser_gen.cpp"
                    break;

                    case 273:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3981 "parser_gen.cpp"
                    break;

                    case 274:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3987 "parser_gen.cpp"
                    break;

                    case 275:
#line 1287 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3993 "parser_gen.cpp"
                    break;

                    case 276:
#line 1291 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4002 "parser_gen.cpp"
                    break;

                    case 277:
#line 1295 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4011 "parser_gen.cpp"
                    break;

                    case 278:
#line 1304 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4019 "parser_gen.cpp"
                    break;

                    case 279:
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4027 "parser_gen.cpp"
                    break;

                    case 280:
#line 1318 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4035 "parser_gen.cpp"
                    break;

                    case 281:
#line 1321 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4044 "parser_gen.cpp"
                    break;

                    case 282:
#line 1328 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4052 "parser_gen.cpp"
                    break;

                    case 283:
#line 1335 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4058 "parser_gen.cpp"
                    break;

                    case 284:
#line 1335 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4064 "parser_gen.cpp"
                    break;

                    case 285:
#line 1335 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4070 "parser_gen.cpp"
                    break;

                    case 286:
#line 1335 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4076 "parser_gen.cpp"
                    break;

                    case 287:
#line 1339 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4084 "parser_gen.cpp"
                    break;

                    case 288:
#line 1345 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{make_vector<std::string>("_id")};
                    }
#line 4092 "parser_gen.cpp"
                    break;

                    case 289:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4098 "parser_gen.cpp"
                    break;

                    case 290:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4104 "parser_gen.cpp"
                    break;

                    case 291:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4110 "parser_gen.cpp"
                    break;

                    case 292:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4116 "parser_gen.cpp"
                    break;

                    case 293:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4122 "parser_gen.cpp"
                    break;

                    case 294:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4128 "parser_gen.cpp"
                    break;

                    case 295:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4134 "parser_gen.cpp"
                    break;

                    case 296:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4140 "parser_gen.cpp"
                    break;

                    case 297:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4146 "parser_gen.cpp"
                    break;

                    case 298:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4152 "parser_gen.cpp"
                    break;

                    case 299:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4158 "parser_gen.cpp"
                    break;

                    case 300:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4164 "parser_gen.cpp"
                    break;

                    case 301:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4170 "parser_gen.cpp"
                    break;

                    case 302:
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4176 "parser_gen.cpp"
                    break;

                    case 303:
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4182 "parser_gen.cpp"
                    break;

                    case 304:
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4188 "parser_gen.cpp"
                    break;

                    case 305:
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4194 "parser_gen.cpp"
                    break;

                    case 306:
#line 1356 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4202 "parser_gen.cpp"
                    break;

                    case 307:
#line 1359 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4210 "parser_gen.cpp"
                    break;

                    case 308:
#line 1362 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4218 "parser_gen.cpp"
                    break;

                    case 309:
#line 1365 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4226 "parser_gen.cpp"
                    break;

                    case 310:
#line 1368 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4234 "parser_gen.cpp"
                    break;

                    case 311:
#line 1371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4242 "parser_gen.cpp"
                    break;

                    case 312:
#line 1374 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4250 "parser_gen.cpp"
                    break;

                    case 313:
#line 1377 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4258 "parser_gen.cpp"
                    break;

                    case 314:
#line 1380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4266 "parser_gen.cpp"
                    break;

                    case 315:
#line 1386 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4275 "parser_gen.cpp"
                    break;

                    case 316:
#line 1393 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4284 "parser_gen.cpp"
                    break;

                    case 317:
#line 1399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4292 "parser_gen.cpp"
                    break;

                    case 318:
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4300 "parser_gen.cpp"
                    break;

                    case 319:
#line 1409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4309 "parser_gen.cpp"
                    break;

                    case 320:
#line 1415 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4317 "parser_gen.cpp"
                    break;

                    case 321:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4325 "parser_gen.cpp"
                    break;

                    case 322:
#line 1425 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4333 "parser_gen.cpp"
                    break;

                    case 323:
#line 1430 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4342 "parser_gen.cpp"
                    break;

                    case 324:
#line 1436 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4350 "parser_gen.cpp"
                    break;

                    case 325:
#line 1441 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4359 "parser_gen.cpp"
                    break;

                    case 326:
#line 1447 "grammar.yy"
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
#line 4371 "parser_gen.cpp"
                    break;

                    case 327:
#line 1456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4380 "parser_gen.cpp"
                    break;

                    case 328:
#line 1462 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4389 "parser_gen.cpp"
                    break;

                    case 329:
#line 1468 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4397 "parser_gen.cpp"
                    break;

                    case 330:
#line 1473 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4406 "parser_gen.cpp"
                    break;

                    case 331:
#line 1479 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4415 "parser_gen.cpp"
                    break;

                    case 332:
#line 1485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4421 "parser_gen.cpp"
                    break;

                    case 333:
#line 1485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4427 "parser_gen.cpp"
                    break;

                    case 334:
#line 1485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4433 "parser_gen.cpp"
                    break;

                    case 335:
#line 1489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4442 "parser_gen.cpp"
                    break;

                    case 336:
#line 1496 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4451 "parser_gen.cpp"
                    break;

                    case 337:
#line 1503 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4460 "parser_gen.cpp"
                    break;

                    case 338:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4466 "parser_gen.cpp"
                    break;

                    case 339:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4472 "parser_gen.cpp"
                    break;

                    case 340:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4478 "parser_gen.cpp"
                    break;

                    case 341:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4484 "parser_gen.cpp"
                    break;

                    case 342:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4490 "parser_gen.cpp"
                    break;

                    case 343:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4496 "parser_gen.cpp"
                    break;

                    case 344:
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4502 "parser_gen.cpp"
                    break;

                    case 345:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4508 "parser_gen.cpp"
                    break;

                    case 346:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4514 "parser_gen.cpp"
                    break;

                    case 347:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4520 "parser_gen.cpp"
                    break;

                    case 348:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4526 "parser_gen.cpp"
                    break;

                    case 349:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4532 "parser_gen.cpp"
                    break;

                    case 350:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4538 "parser_gen.cpp"
                    break;

                    case 351:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4544 "parser_gen.cpp"
                    break;

                    case 352:
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4550 "parser_gen.cpp"
                    break;

                    case 353:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4556 "parser_gen.cpp"
                    break;

                    case 354:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4562 "parser_gen.cpp"
                    break;

                    case 355:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4568 "parser_gen.cpp"
                    break;

                    case 356:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4574 "parser_gen.cpp"
                    break;

                    case 357:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4580 "parser_gen.cpp"
                    break;

                    case 358:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4586 "parser_gen.cpp"
                    break;

                    case 359:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4592 "parser_gen.cpp"
                    break;

                    case 360:
#line 1516 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 4604 "parser_gen.cpp"
                    break;

                    case 361:
#line 1526 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 4612 "parser_gen.cpp"
                    break;

                    case 362:
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4620 "parser_gen.cpp"
                    break;

                    case 363:
#line 1535 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 4628 "parser_gen.cpp"
                    break;

                    case 364:
#line 1538 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4636 "parser_gen.cpp"
                    break;

                    case 365:
#line 1545 "grammar.yy"
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
#line 4646 "parser_gen.cpp"
                    break;

                    case 366:
#line 1554 "grammar.yy"
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
#line 4656 "parser_gen.cpp"
                    break;

                    case 367:
#line 1562 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4664 "parser_gen.cpp"
                    break;

                    case 368:
#line 1565 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4672 "parser_gen.cpp"
                    break;

                    case 369:
#line 1568 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4680 "parser_gen.cpp"
                    break;

                    case 370:
#line 1575 "grammar.yy"
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
#line 4692 "parser_gen.cpp"
                    break;

                    case 371:
#line 1586 "grammar.yy"
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
#line 4704 "parser_gen.cpp"
                    break;

                    case 372:
#line 1596 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4712 "parser_gen.cpp"
                    break;

                    case 373:
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4720 "parser_gen.cpp"
                    break;

                    case 374:
#line 1605 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4730 "parser_gen.cpp"
                    break;

                    case 375:
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4740 "parser_gen.cpp"
                    break;

                    case 376:
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4750 "parser_gen.cpp"
                    break;

                    case 377:
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4758 "parser_gen.cpp"
                    break;

                    case 378:
#line 1632 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4766 "parser_gen.cpp"
                    break;

                    case 379:
#line 1637 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4778 "parser_gen.cpp"
                    break;

                    case 380:
#line 1646 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4786 "parser_gen.cpp"
                    break;

                    case 381:
#line 1652 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4794 "parser_gen.cpp"
                    break;

                    case 382:
#line 1658 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4802 "parser_gen.cpp"
                    break;

                    case 383:
#line 1665 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4813 "parser_gen.cpp"
                    break;

                    case 384:
#line 1675 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4824 "parser_gen.cpp"
                    break;

                    case 385:
#line 1684 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4833 "parser_gen.cpp"
                    break;

                    case 386:
#line 1691 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4842 "parser_gen.cpp"
                    break;

                    case 387:
#line 1698 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4851 "parser_gen.cpp"
                    break;

                    case 388:
#line 1706 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4860 "parser_gen.cpp"
                    break;

                    case 389:
#line 1714 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4869 "parser_gen.cpp"
                    break;

                    case 390:
#line 1722 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4878 "parser_gen.cpp"
                    break;

                    case 391:
#line 1730 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4887 "parser_gen.cpp"
                    break;

                    case 392:
#line 1737 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4895 "parser_gen.cpp"
                    break;

                    case 393:
#line 1743 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4903 "parser_gen.cpp"
                    break;

                    case 394:
#line 1749 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4911 "parser_gen.cpp"
                    break;

                    case 395:
#line 1752 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4919 "parser_gen.cpp"
                    break;

                    case 396:
#line 1758 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4927 "parser_gen.cpp"
                    break;

                    case 397:
#line 1764 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4935 "parser_gen.cpp"
                    break;

                    case 398:
#line 1769 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4943 "parser_gen.cpp"
                    break;

                    case 399:
#line 1772 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4952 "parser_gen.cpp"
                    break;

                    case 400:
#line 1779 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4960 "parser_gen.cpp"
                    break;

                    case 401:
#line 1782 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4968 "parser_gen.cpp"
                    break;

                    case 402:
#line 1785 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4976 "parser_gen.cpp"
                    break;

                    case 403:
#line 1788 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4984 "parser_gen.cpp"
                    break;

                    case 404:
#line 1791 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4992 "parser_gen.cpp"
                    break;

                    case 405:
#line 1794 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 5000 "parser_gen.cpp"
                    break;

                    case 406:
#line 1797 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 5008 "parser_gen.cpp"
                    break;

                    case 407:
#line 1800 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 5016 "parser_gen.cpp"
                    break;

                    case 408:
#line 1805 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5024 "parser_gen.cpp"
                    break;

                    case 409:
#line 1807 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5032 "parser_gen.cpp"
                    break;

                    case 410:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5038 "parser_gen.cpp"
                    break;

                    case 411:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5044 "parser_gen.cpp"
                    break;

                    case 412:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5050 "parser_gen.cpp"
                    break;

                    case 413:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5056 "parser_gen.cpp"
                    break;

                    case 414:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5062 "parser_gen.cpp"
                    break;

                    case 415:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5068 "parser_gen.cpp"
                    break;

                    case 416:
#line 1814 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5074 "parser_gen.cpp"
                    break;

                    case 417:
#line 1818 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5082 "parser_gen.cpp"
                    break;

                    case 418:
#line 1824 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5090 "parser_gen.cpp"
                    break;

                    case 419:
#line 1830 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5099 "parser_gen.cpp"
                    break;

                    case 420:
#line 1838 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setEquals,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5111 "parser_gen.cpp"
                    break;

                    case 421:
#line 1849 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIntersection,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5123 "parser_gen.cpp"
                    break;

                    case 422:
#line 1859 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5132 "parser_gen.cpp"
                    break;

                    case 423:
#line 1867 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setUnion,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>())}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5144 "parser_gen.cpp"
                    break;

                    case 424:
#line 1877 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5150 "parser_gen.cpp"
                    break;

                    case 425:
#line 1877 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5156 "parser_gen.cpp"
                    break;

                    case 426:
#line 1881 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5165 "parser_gen.cpp"
                    break;

                    case 427:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5174 "parser_gen.cpp"
                    break;

                    case 428:
#line 1895 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5180 "parser_gen.cpp"
                    break;

                    case 429:
#line 1895 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5186 "parser_gen.cpp"
                    break;

                    case 430:
#line 1899 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5192 "parser_gen.cpp"
                    break;

                    case 431:
#line 1899 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5198 "parser_gen.cpp"
                    break;

                    case 432:
#line 1903 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5206 "parser_gen.cpp"
                    break;

                    case 433:
#line 1909 "grammar.yy"
                    {
                    }
#line 5212 "parser_gen.cpp"
                    break;

                    case 434:
#line 1910 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 5221 "parser_gen.cpp"
                    break;

                    case 435:
#line 1917 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5229 "parser_gen.cpp"
                    break;

                    case 436:
#line 1923 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5237 "parser_gen.cpp"
                    break;

                    case 437:
#line 1926 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5246 "parser_gen.cpp"
                    break;

                    case 438:
#line 1933 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5254 "parser_gen.cpp"
                    break;

                    case 439:
#line 1940 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5260 "parser_gen.cpp"
                    break;

                    case 440:
#line 1941 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5266 "parser_gen.cpp"
                    break;

                    case 441:
#line 1942 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5272 "parser_gen.cpp"
                    break;

                    case 442:
#line 1943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5278 "parser_gen.cpp"
                    break;

                    case 443:
#line 1944 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5284 "parser_gen.cpp"
                    break;

                    case 444:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5290 "parser_gen.cpp"
                    break;

                    case 445:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5296 "parser_gen.cpp"
                    break;

                    case 446:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5302 "parser_gen.cpp"
                    break;

                    case 447:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5308 "parser_gen.cpp"
                    break;

                    case 448:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5314 "parser_gen.cpp"
                    break;

                    case 449:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5320 "parser_gen.cpp"
                    break;

                    case 450:
#line 1947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5326 "parser_gen.cpp"
                    break;

                    case 451:
#line 1949 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5335 "parser_gen.cpp"
                    break;

                    case 452:
#line 1954 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5344 "parser_gen.cpp"
                    break;

                    case 453:
#line 1959 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5353 "parser_gen.cpp"
                    break;

                    case 454:
#line 1964 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5362 "parser_gen.cpp"
                    break;

                    case 455:
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5371 "parser_gen.cpp"
                    break;

                    case 456:
#line 1974 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5380 "parser_gen.cpp"
                    break;

                    case 457:
#line 1979 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5389 "parser_gen.cpp"
                    break;

                    case 458:
#line 1985 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5395 "parser_gen.cpp"
                    break;

                    case 459:
#line 1986 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5401 "parser_gen.cpp"
                    break;

                    case 460:
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5407 "parser_gen.cpp"
                    break;

                    case 461:
#line 1988 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5413 "parser_gen.cpp"
                    break;

                    case 462:
#line 1989 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5419 "parser_gen.cpp"
                    break;

                    case 463:
#line 1990 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5425 "parser_gen.cpp"
                    break;

                    case 464:
#line 1991 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5431 "parser_gen.cpp"
                    break;

                    case 465:
#line 1992 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5437 "parser_gen.cpp"
                    break;

                    case 466:
#line 1993 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5443 "parser_gen.cpp"
                    break;

                    case 467:
#line 1994 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5449 "parser_gen.cpp"
                    break;

                    case 468:
#line 1999 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 5457 "parser_gen.cpp"
                    break;

                    case 469:
#line 2002 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5465 "parser_gen.cpp"
                    break;

                    case 470:
#line 2009 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 5473 "parser_gen.cpp"
                    break;

                    case 471:
#line 2012 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5481 "parser_gen.cpp"
                    break;

                    case 472:
#line 2019 "grammar.yy"
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
#line 5492 "parser_gen.cpp"
                    break;

                    case 473:
#line 2028 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5500 "parser_gen.cpp"
                    break;

                    case 474:
#line 2033 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5508 "parser_gen.cpp"
                    break;

                    case 475:
#line 2038 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5516 "parser_gen.cpp"
                    break;

                    case 476:
#line 2043 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5524 "parser_gen.cpp"
                    break;

                    case 477:
#line 2048 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5532 "parser_gen.cpp"
                    break;

                    case 478:
#line 2053 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5540 "parser_gen.cpp"
                    break;

                    case 479:
#line 2058 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5548 "parser_gen.cpp"
                    break;

                    case 480:
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5556 "parser_gen.cpp"
                    break;

                    case 481:
#line 2068 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5564 "parser_gen.cpp"
                    break;


#line 5568 "parser_gen.cpp"

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

void ParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string ParserGen::yytnamerr_(const char* yystr) {
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

std::string ParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytnamerr_(yytname_[yysymbol]);
}


// ParserGen::context.
ParserGen::context::context(const ParserGen& yyparser, const symbol_type& yyla)
    : yyparser_(yyparser), yyla_(yyla) {}

int ParserGen::context::expected_tokens(symbol_kind_type yyarg[], int yyargn) const {
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


int ParserGen::yy_syntax_error_arguments_(const context& yyctx,
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
std::string ParserGen::yysyntax_error_(const context& yyctx) const {
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


const short ParserGen::yypact_ninf_ = -733;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    97,   -73,  -45,  -73,  -73,  -40,  85,   -733, -733, -17,  -733, -733, -733, -733, -733, -733,
    972,  36,   52,   598,  -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, 1337,
    -733, -733, -733, -6,   -4,   290,  -2,   8,    290,  -733, 54,   -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, 131,  -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -73,  73,   -733, -733, -733, -733, -733,
    -733, 96,   -733, 112,  17,   -17,  -733, -733, -733, -733, -733, -733, -733, -733, 53,   -733,
    -733, -9,   -733, -733, 1602, 290,  104,  -733, -733, -23,  -733, -92,  -733, -733, -22,  -733,
    1459, -733, -733, 1459, -733, -733, -733, -733, 80,   108,  -733, -733, 82,   55,   -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, 1093,
    464,  -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -3,   -733, -733, 850,  -733, 1093, -733, -733,
    88,   1093, 37,   41,   37,   44,   45,   1093, 45,   47,   57,   -733, -733, -733, 58,   45,
    1093, 1093, 45,   45,   -733, 59,   60,   61,   1093, 62,   1093, 45,   45,   -733, 312,  63,
    67,   45,   70,   37,   71,   -733, -733, -733, -733, -733, 72,   -733, 45,   74,   76,   45,
    77,   81,   83,   1093, 89,   1093, 1093, 90,   93,   102,  105,  1093, 1093, 1093, 1093, 1093,
    1093, 1093, 1093, 1093, 1093, -733, 106,  1093, 1459, 1459, -733, 1623, 114,  12,   1670, -733,
    -733, 132,  139,  1093, 141,  1093, 1093, 155,  163,  169,  1093, 1215, 202,  211,  213,  1093,
    178,  179,  180,  181,  187,  1093, 1093, 1215, 188,  1093, 194,  208,  222,  240,  223,  226,
    228,  231,  232,  233,  234,  237,  238,  1093, 1093, 239,  1093, 241,  1093, 244,  242,  245,
    246,  273,  281,  1093, 240,  251,  1093, 1093, 252,  1093, 1093, 255,  259,  1093, 261,  1093,
    266,  275,  1093, 1093, 1093, 1093, 276,  280,  289,  291,  292,  293,  294,  296,  299,  301,
    240,  1093, 305,  -733, -733, -733, -733, -733, -733, -733, 1093, -733, -733, -733, -733, -733,
    -733, -733, 307,  -733, 308,  1093, -733, -733, -733, 309,  1215, -733, 320,  -733, -733, -733,
    -733, 1093, 1093, 1093, 1093, -733, -733, -733, -733, -733, 1093, 1093, 321,  -733, 1093, -733,
    -733, -733, 1093, 352,  -733, -733, -733, -733, -733, -733, -733, -733, -733, 1093, 1093, -733,
    323,  -733, 1093, -733, 1093, -733, -733, 1093, 1093, 1093, 355,  -733, 1093, 1093, -733, 1093,
    1093, -733, -733, 1093, -733, 1093, -733, -733, 1093, 1093, 1093, 1093, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, 356,  1093, -733, -733, 326,  327,  330,  332,  1215, 334,
    724,  336,  364,  322,  322,  338,  1093, 1093, 340,  344,  -733, 1093, 351,  1093, 357,  353,
    379,  387,  391,  362,  1093, 1093, 1093, 1093, 971,  366,  367,  1093, 1093, 1093, 369,  1093,
    372,  -733, -733, -733, -733, -733, -733, -733, 1215, -733, -733, 1093, 392,  1093, 393,  393,
    374,  1093, 373,  376,  -733, 377,  378,  381,  380,  -733, 382,  1093, 400,  1093, 1093, 383,
    384,  386,  388,  394,  395,  396,  397,  401,  402,  403,  399,  404,  405,  -733, -733, 1093,
    411,  -733, 1093, 364,  392,  -733, -733, 406,  407,  -733, 408,  -733, 409,  -733, -733, 1093,
    418,  426,  -733, 412,  413,  414,  415,  -733, -733, -733, 416,  420,  439,  -733, 443,  -733,
    -733, 1093, -733, 392,  447,  -733, -733, -733, -733, 448,  1093, 1093, -733, -733, -733, -733,
    -733, -733, -733, -733, 456,  457,  458,  -733, 460,  461,  462,  463,  -733, 468,  469,  -733,
    -733, -733, -733};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   0,   77,  3,   8,   2,   5,   4,   398, 6,   1,   0,   0,   0,
    0,   89,  117, 106, 116, 113, 122, 120, 114, 109, 111, 112, 119, 107, 123, 118, 121, 108, 115,
    110, 76,  287, 91,  90,  97,  95,  96,  94,  0,   104, 78,  80,  0,   0,   0,   0,   0,   0,
    10,  0,   12,  13,  14,  15,  16,  17,  7,   148, 124, 187, 126, 188, 125, 149, 131, 164, 127,
    138, 165, 166, 150, 397, 132, 151, 152, 133, 134, 167, 168, 128, 153, 154, 155, 135, 136, 169,
    170, 156, 157, 137, 130, 129, 158, 171, 172, 173, 175, 174, 159, 176, 189, 190, 191, 192, 193,
    160, 177, 161, 98,  101, 102, 103, 100, 99,  180, 178, 179, 181, 182, 183, 162, 139, 140, 141,
    142, 143, 144, 184, 145, 146, 186, 185, 163, 147, 440, 441, 442, 439, 443, 0,   399, 236, 235,
    234, 233, 232, 230, 229, 228, 195, 196, 197, 222, 221, 220, 226, 225, 224, 198, 199, 200, 201,
    202, 83,  203, 194, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 219, 223, 227,
    231, 216, 217, 218, 246, 247, 248, 249, 250, 255, 251, 252, 253, 256, 257, 237, 238, 240, 241,
    242, 254, 243, 244, 245, 81,  239, 79,  0,   0,   21,  22,  23,  24,  26,  28,  0,   25,  0,
    0,   8,   407, 406, 405, 404, 401, 400, 403, 402, 0,   408, 409, 0,   92,  19,  0,   0,   0,
    11,  9,   0,   82,  0,   84,  85,  0,   27,  0,   68,  70,  0,   67,  69,  29,  105, 0,   0,
    394, 395, 0,   0,   86,  88,  93,  61,  60,  57,  56,  59,  53,  52,  55,  45,  44,  47,  49,
    48,  51,  258, 0,   46,  50,  54,  58,  40,  41,  42,  43,  62,  63,  64,  33,  34,  35,  36,
    37,  38,  39,  30,  32,  65,  66,  265, 275, 266, 267, 268, 289, 290, 269, 332, 333, 334, 270,
    424, 425, 273, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353,
    354, 355, 356, 357, 359, 358, 271, 444, 445, 446, 447, 448, 449, 450, 272, 458, 459, 460, 461,
    462, 463, 464, 465, 466, 467, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 274, 410, 411, 412, 413, 414, 415, 416, 31,  18,  0,   396, 83,  280, 260, 258, 262,
    261, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  10,  10,  0,   0,   0,   0,
    0,   0,   288, 0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   0,   0,   0,
    10,  10,  10,  10,  10,  0,   10,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   72,
    0,   0,   0,   0,   259, 278, 0,   0,   0,   0,   0,   0,   0,   0,   0,   258, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   372, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    372, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   372, 0,   0,   75,  74,  71,  73,  20,  87,  279,
    0,   284, 285, 283, 286, 281, 317, 315, 0,   335, 0,   0,   316, 318, 451, 0,   433, 436, 0,
    428, 429, 430, 431, 0,   0,   0,   0,   452, 320, 321, 453, 454, 0,   0,   0,   322, 0,   324,
    455, 456, 0,   0,   306, 307, 308, 309, 310, 311, 312, 313, 314, 0,   0,   457, 0,   336, 0,
    380, 0,   381, 382, 0,   0,   0,   0,   419, 0,   0,   422, 0,   0,   276, 277, 0,   329, 0,
    386, 387, 0,   0,   0,   0,   473, 474, 475, 476, 477, 478, 392, 479, 480, 393, 0,   0,   481,
    282, 0,   0,   0,   0,   433, 0,   0,   0,   468, 361, 361, 0,   367, 367, 0,   0,   373, 0,
    0,   258, 0,   0,   377, 0,   0,   0,   0,   258, 258, 258, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   417, 418, 263, 360, 434, 432, 435, 0,   437, 426, 0,   470, 0,   363, 363, 0,   368,
    0,   0,   427, 0,   0,   0,   0,   337, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   438, 469, 0,   0,   362, 0,   468, 470, 319, 369, 0,
    0,   323, 0,   325, 0,   327, 378, 0,   0,   0,   328, 0,   0,   0,   0,   264, 385, 388, 0,
    0,   0,   330, 0,   331, 471, 0,   364, 470, 0,   370, 371, 374, 326, 0,   0,   0,   375, 420,
    421, 423, 389, 390, 391, 376, 0,   0,   0,   379, 0,   0,   0,   0,   366, 0,   0,   472, 365,
    384, 383};

const short ParserGen::yypgoto_[] = {
    -733, 277,  -733, -733, -21,  -15,  -733, -733, -14,  -13,  -733, -240, -733, -733, 35,   -733,
    -733, -236, -244, -210, -208, -206, -37,  -202, -31,  -46,  -24,  -197, -191, -482, -228, -733,
    -177, -173, -169, -733, -158, -153, -221, -47,  -733, -733, -733, -733, -733, -733, 295,  -733,
    -733, -733, -733, -733, -733, -733, -733, -733, 263,  -430, -733, -43,  -390, -149, -34,  -733,
    -733, -733, -310, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -394, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -733,
    -733, -733, -733, -733, -733, -733, -733, -733, -733, -733, -232, -732, -147, -189, -452, -733,
    -377, -139, -136, -733, -733, -733, -733, -733, -733, -733, -733, 2,    -733, 150,  -733, -733,
    -733, -733, 283,  -733, -733, -733, -733, -733, -733, -733, -733, -733, -53,  -733};

const short ParserGen::yydefgoto_[] = {
    -1,  472, 260, 570, 143, 144, 261, 145, 146, 147, 473, 148, 47,  262, 474, 575, 713, 48,
    194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 670, 205, 206, 207, 208, 209, 210,
    211, 212, 213, 396, 590, 591, 592, 672, 215, 10,  18,  58,  59,  60,  61,  62,  63,  64,
    244, 475, 307, 308, 309, 223, 397, 398, 487, 540, 311, 312, 313, 399, 478, 314, 315, 316,
    317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334,
    525, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351,
    352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369,
    370, 371, 372, 373, 374, 375, 376, 377, 378, 379, 380, 381, 716, 752, 718, 755, 611, 732,
    400, 671, 722, 382, 383, 384, 385, 386, 387, 388, 389, 8,   16,  241, 216, 254, 49,  50,
    252, 253, 51,  14,  19,  239, 240, 268, 149, 6,   526, 228};

const short ParserGen::yytable_[] = {
    214, 44,  45,  46,  227, 11,  12,  221, 263, 588, 221, 481, 293, 226, 269, 293, 219, 488, 265,
    219, 479, 604, 220, 271, 788, 220, 497, 498, 300, 222, 270, 300, 222, 7,   504, 306, 506, 250,
    306, 155, 156, 157, 563, 564, 527, 528, 294, 182, 295, 294, 296, 295, 263, 296, 297, 805, 266,
    297, 568, 298, 9,   542, 298, 544, 545, 299, 13,  251, 299, 550, 551, 552, 553, 554, 555, 556,
    557, 558, 559, 301, 634, 562, 301, 302, 7,   15,  302, 303, 251, 17,  303, 267, 482, 578, 484,
    580, 581, 65,  304, 217, 229, 304, 218, 305, 224, 596, 305, 310, 662, 1,   310, 602, 603, 585,
    225, 606, 2,   3,   4,   243, 245, 246, 5,   247, 249, 523, 391, 392, 393, 621, 622, 251, 624,
    480, 626, 52,  53,  54,  55,  56,  57,  633, 287, 189, 636, 637, 483, 639, 640, 485, 486, 643,
    490, 645, 158, 159, 648, 649, 650, 651, 567, 160, 491, 495, 501, 502, 503, 505, 519, 230, 231,
    663, 520, 232, 233, 522, 524, 531, 576, 534, 665, 535, 537, 167, 168, 577, 538, 579, 541, 234,
    235, 668, 169, 170, 543, 546, 236, 237, 547, 221, 171, 582, 264, 674, 675, 676, 677, 548, 219,
    583, 549, 561, 678, 679, 220, 584, 681, 593, 173, 242, 682, 222, 594, 595, 597, 598, 599, 600,
    293, 293, 749, 684, 685, 601, 605, 174, 687, 238, 688, 263, 607, 689, 690, 691, 300, 300, 693,
    694, 610, 695, 696, 306, 306, 697, 608, 698, 272, 628, 699, 700, 701, 702, 294, 294, 295, 295,
    296, 296, 609, 612, 297, 297, 613, 704, 614, 298, 298, 615, 616, 617, 618, 299, 299, 619, 620,
    623, 631, 625, 721, 721, 627, 629, 630, 726, 632, 301, 301, 635, 638, 302, 302, 641, 736, 303,
    303, 642, 740, 644, 728, 743, 744, 745, 646, 747, 304, 304, 737, 738, 739, 305, 305, 647, 652,
    310, 310, 750, 653, 753, 152, 153, 154, 758, 155, 156, 157, 654, 717, 655, 656, 657, 658, 766,
    659, 768, 769, 660, 476, 661, 161, 162, 163, 664, 666, 667, 669, 164, 165, 166, 492, 493, 494,
    784, 510, 511, 786, 673, 680, 683, 686, 512, 692, 703, 705, 706, 489, 707, 509, 793, 708, 710,
    715, 496, 714, 720, 499, 500, 724, 529, 530, 725, 532, 513, 514, 507, 508, 804, 727, 731, 730,
    521, 515, 516, 733, 729, 808, 809, 734, 735, 517, 751, 533, 741, 742, 536, 746, 539, 560, 748,
    759, 754, 757, 760, 767, 761, 762, 764, 518, 763, 765, 770, 771, 772, 712, 773, 187, 188, 189,
    190, 785, 774, 775, 794, 776, 777, 589, 781, 778, 779, 780, 795, 782, 783, 789, 790, 791, 792,
    589, 571, 796, 797, 798, 799, 800, 572, 573, 574, 801, 401, 402, 403, 404, 405, 21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  802, 34,  35,  36,  803, 37,  38,  406, 806,
    807, 407, 408, 409, 410, 411, 412, 413, 810, 811, 812, 414, 813, 814, 815, 816, 566, 415, 416,
    417, 817, 818, 418, 419, 420, 421, 422, 259, 390, 787, 248, 423, 424, 425, 426, 719, 756, 709,
    427, 428, 429, 430, 431, 432, 433, 589, 434, 435, 436, 723, 477, 437, 438, 439, 440, 441, 442,
    443, 394, 0,   444, 445, 446, 447, 448, 449, 0,   450, 451, 0,   0,   0,   0,   0,   0,   0,
    0,   452, 453, 454, 455, 456, 457, 458, 0,   459, 460, 461, 462, 463, 464, 465, 466, 467, 468,
    469, 470, 471, 257, 258, 0,   0,   0,   0,   0,   0,   0,   66,  67,  68,  69,  70,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  589, 37,  38,  71,
    0,   0,   72,  73,  74,  75,  76,  77,  78,  0,   0,   0,   79,  0,   0,   0,   0,   80,  81,
    82,  83,  0,   0,   84,  85,  40,  86,  87,  0,   0,   0,   0,   88,  89,  90,  91,  0,   0,
    589, 92,  93,  94,  95,  96,  97,  98,  0,   99,  100, 101, 0,   0,   102, 103, 104, 105, 106,
    107, 108, 0,   0,   109, 110, 111, 112, 113, 114, 0,   115, 116, 117, 118, 119, 120, 121, 122,
    0,   0,   123, 124, 125, 126, 127, 128, 129, 0,   130, 131, 132, 133, 134, 135, 136, 137, 138,
    139, 140, 141, 142, 43,  66,  67,  68,  69,  70,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  71,  0,   0,   72,  73,  74,  75,  76,
    77,  78,  0,   0,   0,   79,  0,   0,   0,   0,   711, 81,  82,  83,  0,   0,   84,  85,  40,
    86,  87,  0,   0,   0,   0,   88,  89,  90,  91,  0,   0,   0,   92,  93,  94,  95,  96,  97,
    98,  0,   99,  100, 101, 0,   0,   102, 103, 104, 105, 106, 107, 108, 0,   0,   109, 110, 111,
    112, 113, 114, 0,   115, 116, 117, 118, 119, 120, 121, 122, 0,   0,   123, 124, 125, 126, 127,
    128, 129, 0,   130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 43,  401, 402,
    403, 404, 405, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   406, 0,   0,   407, 408, 409, 410, 411, 412, 413, 0,   0,   0,   414, 0,
    0,   0,   0,   0,   415, 416, 417, 0,   0,   418, 419, 0,   421, 422, 0,   0,   0,   0,   423,
    424, 425, 426, 0,   0,   0,   427, 428, 429, 430, 431, 432, 433, 0,   434, 435, 436, 0,   0,
    437, 438, 439, 440, 441, 442, 443, 0,   0,   444, 445, 446, 447, 448, 449, 0,   450, 451, 0,
    0,   0,   0,   0,   0,   0,   0,   452, 453, 454, 455, 456, 457, 458, 0,   459, 460, 461, 462,
    463, 464, 465, 466, 467, 468, 469, 470, 471, 20,  0,   21,  22,  23,  24,  25,  26,  27,  28,
    29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  150, 151, 0,   0,   0,   0,   0,
    0,   0,   152, 153, 154, 0,   155, 156, 157, 707, 0,   39,  0,   0,   158, 159, 0,   0,   0,
    40,  0,   160, 161, 162, 163, 0,   0,   0,   0,   164, 165, 166, 0,   0,   0,   0,   0,   0,
    0,   0,   41,  0,   42,  167, 168, 0,   0,   0,   0,   0,   0,   0,   169, 170, 0,   0,   0,
    0,   0,   0,   171, 0,   0,   0,   0,   0,   0,   0,   0,   287, 395, 0,   0,   0,   0,   0,
    0,   0,   173, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   43,  0,
    174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192,
    193, 150, 151, 0,   0,   0,   0,   0,   0,   0,   152, 153, 154, 0,   155, 156, 157, 0,   0,
    0,   0,   0,   158, 159, 0,   0,   0,   0,   0,   160, 161, 162, 163, 0,   0,   0,   0,   164,
    165, 166, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   167, 168, 0,   0,   0,   0,
    0,   0,   0,   169, 170, 0,   0,   0,   0,   0,   0,   171, 0,   0,   0,   0,   0,   0,   0,
    0,   287, 395, 0,   0,   0,   0,   0,   0,   0,   173, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184,
    185, 186, 187, 188, 189, 190, 191, 192, 193, 150, 151, 0,   0,   0,   0,   0,   0,   0,   152,
    153, 154, 0,   155, 156, 157, 0,   0,   0,   0,   0,   158, 159, 0,   0,   0,   0,   0,   160,
    161, 162, 163, 0,   0,   0,   0,   164, 165, 166, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   167, 168, 0,   0,   0,   0,   0,   0,   0,   169, 170, 0,   0,   0,   0,   0,   0,
    171, 0,   0,   0,   0,   0,   0,   0,   0,   586, 587, 0,   0,   0,   0,   0,   0,   0,   173,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   174, 175, 176,
    177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 150, 151,
    0,   0,   0,   0,   0,   0,   0,   152, 153, 154, 0,   155, 156, 157, 0,   0,   0,   0,   0,
    158, 159, 0,   0,   0,   0,   0,   160, 161, 162, 163, 0,   0,   0,   0,   164, 165, 166, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   167, 168, 0,   0,   0,   0,   0,   0,   0,
    169, 170, 0,   0,   0,   0,   0,   0,   171, 0,   0,   0,   0,   0,   0,   0,   0,   0,   172,
    0,   0,   0,   0,   0,   0,   0,   173, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187,
    188, 189, 190, 191, 192, 193, 273, 274, 0,   0,   0,   0,   0,   0,   0,   275, 276, 277, 0,
    278, 279, 280, 0,   0,   0,   0,   0,   158, 159, 0,   0,   0,   0,   0,   160, 281, 282, 283,
    0,   0,   0,   0,   284, 285, 286, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   167,
    168, 0,   0,   0,   0,   0,   0,   0,   169, 170, 0,   0,   0,   0,   0,   0,   171, 0,   0,
    0,   0,   0,   0,   0,   0,   287, 288, 0,   0,   0,   0,   0,   0,   0,   173, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   174, 0,   0,   177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 289, 290, 291, 292, 191, 192, 193, 21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  0,   21,  22,  23,
    24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  255, 37,  38,  0,   0,
    0,   0,   0,   256, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   565, 0,   0,
    0,   0,   0,   0,   0,   420, 21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,
    0,   34,  35,  36,  0,   37,  38,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   569, 0,   0,   0,   0,   0,   0,   0,   40,  0,   0,   0,   0,
    0,   257, 258, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   257, 258, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   117, 118, 119, 120, 121, 122, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   43};

const short ParserGen::yycheck_[] = {
    47,  16,  16,  16,  57,  3,   4,   53,  244, 491, 56,  401, 256, 56,  106, 259, 53,  407, 246,
    56,  397, 503, 53,  45,  756, 56,  416, 417, 256, 53,  251, 259, 56,  106, 424, 256, 426, 46,
    259, 42,  43,  44,  472, 473, 438, 439, 256, 139, 256, 259, 256, 259, 288, 259, 256, 787, 79,
    259, 46,  256, 105, 451, 259, 453, 454, 256, 106, 76,  259, 459, 460, 461, 462, 463, 464, 465,
    466, 467, 468, 256, 532, 471, 259, 256, 106, 0,   259, 256, 76,  106, 259, 114, 402, 483, 404,
    485, 486, 45,  256, 105, 46,  259, 106, 256, 106, 495, 259, 256, 560, 12,  259, 501, 502, 490,
    106, 505, 19,  20,  21,  46,  24,  9,   25,  106, 71,  435, 46,  19,  46,  519, 520, 76,  522,
    45,  524, 99,  100, 101, 102, 103, 104, 531, 105, 146, 534, 535, 105, 537, 538, 105, 105, 541,
    105, 543, 50,  51,  546, 547, 548, 549, 46,  57,  105, 105, 105, 105, 105, 105, 105, 38,  39,
    561, 105, 42,  43,  105, 105, 105, 46,  105, 570, 105, 105, 79,  80,  46,  105, 46,  105, 58,
    59,  581, 88,  89,  105, 105, 65,  66,  105, 245, 96,  46,  245, 593, 594, 595, 596, 105, 245,
    46,  105, 105, 602, 603, 245, 46,  606, 15,  114, 217, 610, 245, 11,  10,  46,  46,  46,  46,
    472, 473, 712, 621, 622, 46,  46,  131, 626, 106, 628, 475, 46,  631, 632, 633, 472, 473, 636,
    637, 8,   639, 640, 472, 473, 643, 46,  645, 254, 15,  648, 649, 650, 651, 472, 473, 472, 473,
    472, 473, 46,  46,  472, 473, 46,  663, 46,  472, 473, 46,  46,  46,  46,  472, 473, 46,  46,
    46,  13,  46,  678, 679, 46,  46,  46,  683, 13,  472, 473, 46,  46,  472, 473, 46,  692, 472,
    473, 46,  696, 46,  685, 699, 700, 701, 46,  703, 472, 473, 693, 694, 695, 472, 473, 46,  46,
    472, 473, 715, 46,  717, 38,  39,  40,  721, 42,  43,  44,  46,  14,  46,  46,  46,  46,  731,
    46,  733, 734, 46,  392, 46,  58,  59,  60,  46,  45,  45,  45,  65,  66,  67,  411, 412, 413,
    751, 50,  51,  754, 45,  45,  15,  45,  57,  15,  15,  46,  46,  408, 45,  429, 767, 46,  45,
    16,  415, 46,  45,  418, 419, 46,  440, 441, 45,  443, 79,  80,  427, 428, 785, 45,  18,  45,
    433, 88,  89,  15,  46,  794, 795, 15,  45,  96,  17,  444, 45,  45,  447, 45,  449, 469, 45,
    45,  26,  46,  45,  22,  46,  46,  45,  114, 46,  46,  46,  46,  45,  672, 45,  144, 145, 146,
    147, 27,  45,  45,  23,  46,  46,  491, 46,  45,  45,  45,  23,  46,  46,  46,  46,  46,  46,
    503, 478, 46,  46,  46,  46,  46,  478, 478, 478, 46,  3,   4,   5,   6,   7,   8,   9,   10,
    11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  46,  22,  23,  24,  46,  26,  27,  28,  46,
    46,  31,  32,  33,  34,  35,  36,  37,  46,  46,  46,  41,  46,  46,  46,  46,  475, 47,  48,
    49,  46,  46,  52,  53,  54,  55,  56,  244, 259, 755, 229, 61,  62,  63,  64,  676, 719, 670,
    68,  69,  70,  71,  72,  73,  74,  586, 76,  77,  78,  679, 394, 81,  82,  83,  84,  85,  86,
    87,  269, -1,  90,  91,  92,  93,  94,  95,  -1,  97,  98,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  107, 108, 109, 110, 111, 112, 113, -1,  115, 116, 117, 118, 119, 120, 121, 122, 123, 124,
    125, 126, 127, 128, 129, -1,  -1,  -1,  -1,  -1,  -1,  -1,  3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,  670, 26,  27,  28,
    -1,  -1,  31,  32,  33,  34,  35,  36,  37,  -1,  -1,  -1,  41,  -1,  -1,  -1,  -1,  46,  47,
    48,  49,  -1,  -1,  52,  53,  54,  55,  56,  -1,  -1,  -1,  -1,  61,  62,  63,  64,  -1,  -1,
    712, 68,  69,  70,  71,  72,  73,  74,  -1,  76,  77,  78,  -1,  -1,  81,  82,  83,  84,  85,
    86,  87,  -1,  -1,  90,  91,  92,  93,  94,  95,  -1,  97,  98,  99,  100, 101, 102, 103, 104,
    -1,  -1,  107, 108, 109, 110, 111, 112, 113, -1,  115, 116, 117, 118, 119, 120, 121, 122, 123,
    124, 125, 126, 127, 128, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,  27,  28,  -1,  -1,  31,  32,  33,  34,  35,
    36,  37,  -1,  -1,  -1,  41,  -1,  -1,  -1,  -1,  46,  47,  48,  49,  -1,  -1,  52,  53,  54,
    55,  56,  -1,  -1,  -1,  -1,  61,  62,  63,  64,  -1,  -1,  -1,  68,  69,  70,  71,  72,  73,
    74,  -1,  76,  77,  78,  -1,  -1,  81,  82,  83,  84,  85,  86,  87,  -1,  -1,  90,  91,  92,
    93,  94,  95,  -1,  97,  98,  99,  100, 101, 102, 103, 104, -1,  -1,  107, 108, 109, 110, 111,
    112, 113, -1,  115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 3,   4,
    5,   6,   7,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  28,  -1,  -1,  31,  32,  33,  34,  35,  36,  37,  -1,  -1,  -1,  41,  -1,
    -1,  -1,  -1,  -1,  47,  48,  49,  -1,  -1,  52,  53,  -1,  55,  56,  -1,  -1,  -1,  -1,  61,
    62,  63,  64,  -1,  -1,  -1,  68,  69,  70,  71,  72,  73,  74,  -1,  76,  77,  78,  -1,  -1,
    81,  82,  83,  84,  85,  86,  87,  -1,  -1,  90,  91,  92,  93,  94,  95,  -1,  97,  98,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  107, 108, 109, 110, 111, 112, 113, -1,  115, 116, 117, 118,
    119, 120, 121, 122, 123, 124, 125, 126, 127, 6,   -1,  8,   9,   10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,  27,  29,  30,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  38,  39,  40,  -1,  42,  43,  44,  45,  -1,  46,  -1,  -1,  50,  51,  -1,  -1,  -1,
    54,  -1,  57,  58,  59,  60,  -1,  -1,  -1,  -1,  65,  66,  67,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  75,  -1,  77,  79,  80,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  88,  89,  -1,  -1,  -1,
    -1,  -1,  -1,  96,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  105, 106, -1,  -1,  -1,  -1,  -1,
    -1,  -1,  114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  128, -1,
    131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,  39,  40,  -1,  42,  43,  44,  -1,  -1,
    -1,  -1,  -1,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  59,  60,  -1,  -1,  -1,  -1,  65,
    66,  67,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  79,  80,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  88,  89,  -1,  -1,  -1,  -1,  -1,  -1,  96,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141,
    142, 143, 144, 145, 146, 147, 148, 149, 150, 29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,
    39,  40,  -1,  42,  43,  44,  -1,  -1,  -1,  -1,  -1,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,
    58,  59,  60,  -1,  -1,  -1,  -1,  65,  66,  67,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  79,  80,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  88,  89,  -1,  -1,  -1,  -1,  -1,  -1,
    96,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  114,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  131, 132, 133,
    134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 29,  30,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,  39,  40,  -1,  42,  43,  44,  -1,  -1,  -1,  -1,  -1,
    50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  59,  60,  -1,  -1,  -1,  -1,  65,  66,  67,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  79,  80,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    88,  89,  -1,  -1,  -1,  -1,  -1,  -1,  96,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  106,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
    145, 146, 147, 148, 149, 150, 29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,  39,  40,  -1,
    42,  43,  44,  -1,  -1,  -1,  -1,  -1,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  59,  60,
    -1,  -1,  -1,  -1,  65,  66,  67,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  79,
    80,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  88,  89,  -1,  -1,  -1,  -1,  -1,  -1,  96,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  131, -1,  -1,  134, 135, 136,
    137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,  27,  -1,  8,   9,   10,
    11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,  46,  26,  27,  -1,  -1,
    -1,  -1,  -1,  54,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  46,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  54,  8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    -1,  22,  23,  24,  -1,  26,  27,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  46,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  54,  -1,  -1,  -1,  -1,
    -1,  128, 129, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  128, 129, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  99,  100, 101, 102, 103, 104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  128};

const short ParserGen::yystos_[] = {
    0,   12,  19,  20,  21,  25,  325, 106, 309, 105, 199, 309, 309, 106, 319, 0,   310, 106, 200,
    320, 6,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  22,  23,  24,  26,
    27,  46,  54,  75,  77,  128, 159, 162, 163, 166, 171, 314, 315, 318, 99,  100, 101, 102, 103,
    104, 201, 202, 203, 204, 205, 206, 207, 45,  3,   4,   5,   6,   7,   28,  31,  32,  33,  34,
    35,  36,  37,  41,  46,  47,  48,  49,  52,  53,  55,  56,  61,  62,  63,  64,  68,  69,  70,
    71,  72,  73,  74,  76,  77,  78,  81,  82,  83,  84,  85,  86,  87,  90,  91,  92,  93,  94,
    95,  97,  98,  99,  100, 101, 102, 103, 104, 107, 108, 109, 110, 111, 112, 113, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 158, 159, 161, 162, 163, 165, 324, 29,  30,
    38,  39,  40,  42,  43,  44,  50,  51,  57,  58,  59,  60,  65,  66,  67,  79,  80,  88,  89,
    96,  106, 114, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146,
    147, 148, 149, 150, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 184, 185, 186, 187,
    188, 189, 190, 191, 192, 193, 198, 312, 105, 106, 176, 178, 179, 180, 213, 106, 106, 213, 326,
    327, 46,  38,  39,  42,  43,  58,  59,  65,  66,  106, 321, 322, 311, 309, 46,  208, 24,  9,
    106, 200, 71,  46,  76,  316, 317, 313, 46,  54,  128, 129, 155, 156, 160, 167, 171, 213, 184,
    79,  114, 323, 106, 192, 45,  309, 29,  30,  38,  39,  40,  42,  43,  44,  58,  59,  60,  65,
    66,  67,  105, 106, 144, 145, 146, 147, 172, 173, 174, 175, 177, 181, 182, 184, 186, 187, 188,
    190, 191, 192, 210, 211, 212, 215, 218, 219, 220, 223, 224, 225, 226, 227, 228, 229, 230, 231,
    232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 245, 246, 247, 248, 249, 250, 251,
    252, 253, 254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270,
    271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289,
    290, 291, 301, 302, 303, 304, 305, 306, 307, 308, 210, 46,  19,  46,  316, 106, 193, 214, 215,
    221, 298, 3,   4,   5,   6,   7,   28,  31,  32,  33,  34,  35,  36,  37,  41,  47,  48,  49,
    52,  53,  54,  55,  56,  61,  62,  63,  64,  68,  69,  70,  71,  72,  73,  74,  76,  77,  78,
    81,  82,  83,  84,  85,  86,  87,  90,  91,  92,  93,  94,  95,  97,  98,  107, 108, 109, 110,
    111, 112, 113, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 155, 164, 168,
    209, 179, 311, 222, 298, 45,  214, 220, 105, 220, 105, 105, 216, 214, 216, 105, 105, 326, 326,
    326, 105, 216, 214, 214, 216, 216, 105, 105, 105, 214, 105, 214, 216, 216, 326, 50,  51,  57,
    79,  80,  88,  89,  96,  114, 105, 105, 216, 105, 220, 105, 244, 326, 244, 244, 326, 326, 105,
    326, 216, 105, 105, 216, 105, 105, 216, 217, 105, 214, 105, 214, 214, 105, 105, 105, 105, 214,
    214, 214, 214, 214, 214, 214, 214, 214, 214, 326, 105, 214, 211, 211, 46,  168, 46,  46,  46,
    157, 158, 159, 162, 163, 169, 46,  46,  214, 46,  214, 214, 46,  46,  46,  298, 105, 106, 183,
    193, 194, 195, 196, 15,  11,  10,  214, 46,  46,  46,  46,  46,  214, 214, 183, 46,  214, 46,
    46,  46,  8,   296, 46,  46,  46,  46,  46,  46,  46,  46,  46,  214, 214, 46,  214, 46,  214,
    46,  15,  46,  46,  13,  13,  214, 296, 46,  214, 214, 46,  214, 214, 46,  46,  214, 46,  214,
    46,  46,  214, 214, 214, 214, 46,  46,  46,  46,  46,  46,  46,  46,  46,  46,  296, 214, 46,
    214, 45,  45,  214, 45,  183, 299, 197, 45,  214, 214, 214, 214, 214, 214, 45,  214, 214, 15,
    214, 214, 45,  214, 214, 214, 214, 214, 15,  214, 214, 214, 214, 214, 214, 214, 214, 214, 214,
    15,  214, 46,  46,  45,  46,  299, 45,  46,  165, 170, 46,  16,  292, 14,  294, 294, 45,  214,
    300, 300, 46,  45,  214, 45,  298, 46,  45,  18,  297, 15,  15,  45,  214, 298, 298, 298, 214,
    45,  45,  214, 214, 214, 45,  214, 45,  183, 214, 17,  293, 214, 26,  295, 295, 46,  214, 45,
    45,  46,  46,  46,  45,  46,  214, 22,  214, 214, 46,  46,  45,  45,  45,  45,  46,  46,  45,
    45,  45,  46,  46,  46,  214, 27,  214, 292, 293, 46,  46,  46,  46,  214, 23,  23,  46,  46,
    46,  46,  46,  46,  46,  46,  214, 293, 46,  46,  214, 214, 46,  46,  46,  46,  46,  46,  46,
    46,  46};

const short ParserGen::yyr1_[] = {
    0,   154, 325, 325, 325, 325, 325, 199, 200, 200, 327, 326, 201, 201, 201, 201, 201, 201, 207,
    202, 203, 213, 213, 213, 213, 204, 205, 206, 208, 208, 167, 167, 210, 211, 211, 211, 211, 211,
    211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211,
    211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 155, 156, 156, 156, 212, 209, 209, 168, 168,
    309, 310, 310, 314, 314, 312, 312, 311, 311, 316, 317, 317, 315, 318, 318, 318, 313, 313, 166,
    166, 166, 162, 158, 158, 158, 158, 158, 158, 159, 160, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 161, 161, 161, 161, 161, 161, 161, 161, 161,
    161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161,
    161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161,
    161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161, 161,
    161, 161, 161, 161, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 185, 198, 186, 187, 188,
    190, 191, 192, 172, 173, 174, 175, 177, 181, 182, 176, 176, 176, 176, 178, 178, 178, 178, 179,
    179, 179, 179, 180, 180, 180, 180, 189, 189, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193,
    193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 193, 298, 298, 214, 214, 214, 216, 217, 215,
    215, 215, 215, 215, 215, 215, 215, 215, 215, 218, 219, 219, 220, 221, 222, 222, 169, 157, 157,
    157, 157, 163, 164, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223, 223,
    223, 223, 224, 224, 224, 224, 224, 224, 224, 224, 224, 225, 226, 277, 278, 279, 280, 281, 282,
    283, 284, 285, 286, 287, 288, 289, 290, 291, 227, 227, 227, 228, 229, 230, 234, 234, 234, 234,
    234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 235,
    294, 294, 295, 295, 236, 237, 300, 300, 300, 238, 239, 296, 296, 240, 247, 257, 297, 297, 244,
    241, 242, 243, 245, 246, 248, 249, 250, 251, 252, 253, 254, 255, 256, 323, 323, 321, 319, 320,
    320, 322, 322, 322, 322, 322, 322, 322, 322, 324, 324, 301, 301, 301, 301, 301, 301, 301, 302,
    303, 304, 305, 306, 307, 308, 231, 231, 232, 233, 183, 183, 194, 194, 195, 299, 299, 196, 197,
    197, 170, 165, 165, 165, 165, 165, 258, 258, 258, 258, 258, 258, 258, 259, 260, 261, 262, 263,
    264, 265, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 292, 292, 293, 293, 267, 268, 269,
    270, 271, 272, 273, 274, 275, 276};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 2, 2,  3,  0,  4, 0, 2, 1, 1, 1, 1, 1, 1,  5,  3, 7, 1, 1, 1, 1, 2, 2, 4, 0,
    2, 2, 2, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 3, 1, 2, 2, 2,  3,  0, 2, 2, 1, 1, 3, 0, 2, 1, 2,
    5, 5, 1, 1, 1, 0, 2,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 0, 2, 1,
    1, 1, 4, 5, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 3,  3,  0, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 4, 4,  4,  4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    7, 4, 4, 4, 7, 4, 7,  8,  7,  7, 4, 7, 7, 1, 1, 1, 4, 4,  6,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8,
    0, 2, 7, 4, 4, 4, 11, 11, 7,  4, 4, 7, 8, 8, 8, 4, 4, 1,  1,  4, 3, 0, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 2, 2, 1, 1, 1,  1,  1,  1, 1, 6, 6, 4, 8, 8, 4, 8,  1,  1, 6, 6, 1, 1, 1, 1, 3, 0, 2,
    3, 0, 2, 2, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 4, 4,  4,  4, 4, 4, 4, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 0,  2,  11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


#if YYDEBUG || 1
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const ParserGen::yytname_[] = {"\"EOF\"",
                                           "error",
                                           "\"invalid token\"",
                                           "ABS",
                                           "ADD",
                                           "\"allElementsTrue\"",
                                           "AND",
                                           "\"anyElementTrue\"",
                                           "\"chars argument\"",
                                           "\"coll argument\"",
                                           "\"date argument\"",
                                           "\"dateString argument\"",
                                           "\"filter\"",
                                           "\"find argument\"",
                                           "\"format argument\"",
                                           "\"input argument\"",
                                           "\"onError argument\"",
                                           "\"onNull argument\"",
                                           "\"options argument\"",
                                           "\"pipeline argument\"",
                                           "\"q\"",
                                           "\"query\"",
                                           "\"regex argument\"",
                                           "\"replacement argument\"",
                                           "\"size argument\"",
                                           "\"sort argument\"",
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
                                           "\"-1 (decimal)\"",
                                           "\"1 (decimal)\"",
                                           "\"zero (decimal)\"",
                                           "DIVIDE",
                                           "\"-1 (double)\"",
                                           "\"1 (double)\"",
                                           "\"zero (double)\"",
                                           "\"end of array\"",
                                           "\"end of object\"",
                                           "EQ",
                                           "EXPONENT",
                                           "FLOOR",
                                           "\"geoNearDistance\"",
                                           "\"geoNearPoint\"",
                                           "GT",
                                           "GTE",
                                           "ID",
                                           "INDEX_OF_BYTES",
                                           "INDEX_OF_CP",
                                           "\"indexKey\"",
                                           "\"-1 (int)\"",
                                           "\"1 (int)\"",
                                           "\"zero (int)\"",
                                           "LITERAL",
                                           "LN",
                                           "LOG",
                                           "LOGTEN",
                                           "\"-1 (long)\"",
                                           "\"1 (long)\"",
                                           "\"zero (long)\"",
                                           "LT",
                                           "LTE",
                                           "LTRIM",
                                           "META",
                                           "MOD",
                                           "MULTIPLY",
                                           "NE",
                                           "NOR",
                                           "NOT",
                                           "OR",
                                           "POW",
                                           "\"randVal\"",
                                           "\"recordId\"",
                                           "REGEX_FIND",
                                           "REGEX_FIND_ALL",
                                           "REGEX_MATCH",
                                           "REPLACE_ALL",
                                           "REPLACE_ONE",
                                           "ROUND",
                                           "RTRIM",
                                           "\"searchHighlights\"",
                                           "\"searchScore\"",
                                           "\"setDifference\"",
                                           "\"setEquals\"",
                                           "\"setIntersection\"",
                                           "\"setIsSubset\"",
                                           "\"setUnion\"",
                                           "\"slice\"",
                                           "\"sortKey\"",
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
                                           "\"textScore\"",
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
                                           "\"fieldname containing dotted path\"",
                                           "\"$-prefixed fieldname\"",
                                           "\"string\"",
                                           "\"$-prefixed string\"",
                                           "\"$$-prefixed string\"",
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
                                           "\"arbitrary integer\"",
                                           "\"arbitrary long\"",
                                           "\"arbitrary double\"",
                                           "\"arbitrary decimal\"",
                                           "\"Timestamp\"",
                                           "\"minKey\"",
                                           "\"maxKey\"",
                                           "START_PIPELINE",
                                           "START_MATCH",
                                           "START_SORT",
                                           "$accept",
                                           "aggregationProjectionFieldname",
                                           "projectionFieldname",
                                           "expressionFieldname",
                                           "stageAsUserFieldname",
                                           "argAsUserFieldname",
                                           "argAsProjectionPath",
                                           "aggExprAsUserFieldname",
                                           "invariableUserFieldname",
                                           "idAsUserFieldname",
                                           "idAsProjectionPath",
                                           "valueFieldname",
                                           "predFieldname",
                                           "projectField",
                                           "projectionObjectField",
                                           "expressionField",
                                           "valueField",
                                           "arg",
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
                                           "aggregationFieldPath",
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
                                           "pipeline",
                                           "stageList",
                                           "stage",
                                           "inhibitOptimization",
                                           "unionWith",
                                           "skip",
                                           "limit",
                                           "project",
                                           "sample",
                                           "projectFields",
                                           "projectionObjectFields",
                                           "topLevelProjection",
                                           "projection",
                                           "projectionObject",
                                           "num",
                                           "expression",
                                           "compoundNonObjectExpression",
                                           "exprFixedTwoArg",
                                           "exprFixedThreeArg",
                                           "arrayManipulation",
                                           "slice",
                                           "expressionArray",
                                           "expressionObject",
                                           "expressionFields",
                                           "maths",
                                           "meta",
                                           "add",
                                           "atan2",
                                           "boolExprs",
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
                                           "setExpression",
                                           "allElementsTrue",
                                           "anyElementTrue",
                                           "setDifference",
                                           "setEquals",
                                           "setIntersection",
                                           "setIsSubset",
                                           "setUnion",
                                           "match",
                                           "predicates",
                                           "compoundMatchExprs",
                                           "predValue",
                                           "additionalExprs",
                                           "predicate",
                                           "logicalExpr",
                                           "operatorExpression",
                                           "notExpr",
                                           "logicalExprField",
                                           "sortSpecs",
                                           "specList",
                                           "metaSort",
                                           "oneOrNegOne",
                                           "metaSortKeyword",
                                           "sortSpec",
                                           "start",
                                           "START_ORDERED_OBJECT",
                                           "$@1",
                                           YY_NULLPTR};
#endif


#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    333,  333,  336,  339,  342,  345,  352,  358,  359,  367,  367,  370,  370,  370,  370,
    370,  370,  373,  383,  389,  399,  399,  399,  399,  403,  408,  413,  432,  435,  442,  445,
    451,  465,  466,  467,  468,  469,  470,  471,  472,  473,  474,  475,  476,  479,  482,  485,
    488,  491,  494,  497,  500,  503,  506,  509,  512,  515,  518,  521,  524,  527,  530,  531,
    532,  533,  534,  539,  547,  560,  561,  578,  585,  589,  597,  600,  606,  612,  615,  621,
    624,  633,  634,  640,  643,  650,  653,  657,  666,  674,  675,  676,  679,  682,  689,  689,
    689,  692,  700,  703,  706,  709,  712,  715,  721,  727,  746,  749,  752,  755,  758,  761,
    764,  767,  770,  773,  776,  779,  782,  785,  788,  791,  794,  797,  805,  808,  811,  814,
    817,  820,  823,  826,  829,  832,  835,  838,  841,  844,  847,  850,  853,  856,  859,  862,
    865,  868,  871,  874,  877,  880,  883,  886,  889,  892,  895,  898,  901,  904,  907,  910,
    913,  916,  919,  922,  925,  928,  931,  934,  937,  940,  943,  946,  949,  952,  955,  958,
    961,  964,  967,  970,  973,  976,  979,  982,  985,  988,  991,  994,  997,  1000, 1003, 1006,
    1009, 1012, 1019, 1024, 1027, 1030, 1033, 1036, 1039, 1042, 1045, 1048, 1054, 1068, 1082, 1088,
    1094, 1100, 1106, 1112, 1118, 1124, 1130, 1136, 1142, 1148, 1154, 1160, 1163, 1166, 1169, 1175,
    1178, 1181, 1184, 1190, 1193, 1196, 1199, 1205, 1208, 1211, 1214, 1220, 1223, 1229, 1230, 1231,
    1232, 1233, 1234, 1235, 1236, 1237, 1238, 1239, 1240, 1241, 1242, 1243, 1244, 1245, 1246, 1247,
    1248, 1249, 1256, 1257, 1264, 1264, 1264, 1269, 1276, 1282, 1282, 1282, 1282, 1282, 1283, 1283,
    1283, 1283, 1283, 1287, 1291, 1295, 1304, 1312, 1318, 1321, 1328, 1335, 1335, 1335, 1335, 1339,
    1345, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1351, 1352, 1352,
    1352, 1352, 1356, 1359, 1362, 1365, 1368, 1371, 1374, 1377, 1380, 1386, 1393, 1399, 1404, 1409,
    1415, 1420, 1425, 1430, 1436, 1441, 1447, 1456, 1462, 1468, 1473, 1479, 1485, 1485, 1485, 1489,
    1496, 1503, 1510, 1510, 1510, 1510, 1510, 1510, 1510, 1511, 1511, 1511, 1511, 1511, 1511, 1511,
    1511, 1512, 1512, 1512, 1512, 1512, 1512, 1512, 1516, 1526, 1529, 1535, 1538, 1544, 1553, 1562,
    1565, 1568, 1574, 1585, 1596, 1599, 1605, 1613, 1621, 1629, 1632, 1637, 1646, 1652, 1658, 1664,
    1674, 1684, 1691, 1698, 1705, 1713, 1721, 1729, 1737, 1743, 1749, 1752, 1758, 1764, 1769, 1772,
    1779, 1782, 1785, 1788, 1791, 1794, 1797, 1800, 1805, 1807, 1813, 1813, 1813, 1813, 1813, 1813,
    1814, 1818, 1824, 1830, 1837, 1848, 1859, 1866, 1877, 1877, 1881, 1888, 1895, 1895, 1899, 1899,
    1903, 1909, 1910, 1917, 1923, 1926, 1933, 1940, 1941, 1942, 1943, 1944, 1947, 1947, 1947, 1947,
    1947, 1947, 1947, 1949, 1954, 1959, 1964, 1969, 1974, 1979, 1985, 1986, 1987, 1988, 1989, 1990,
    1991, 1992, 1993, 1994, 1999, 2002, 2009, 2012, 2018, 2028, 2033, 2038, 2043, 2048, 2053, 2058,
    2063, 2068};

void ParserGen::yy_stack_print_() const {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

void ParserGen::yy_reduce_print_(int yyrule) const {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 57 "grammar.yy"
}  // namespace mongo
#line 6872 "parser_gen.cpp"

#line 2072 "grammar.yy"
