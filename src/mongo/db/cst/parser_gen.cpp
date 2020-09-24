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
        case 174:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 181:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 183:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 180:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 179:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 182:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 215:  // dbPointer
        case 216:  // javascript
        case 217:  // symbol
        case 218:  // javascriptWScope
        case 219:  // int
        case 220:  // timestamp
        case 221:  // long
        case 222:  // double
        case 223:  // decimal
        case 224:  // minKey
        case 225:  // maxKey
        case 226:  // value
        case 227:  // string
        case 228:  // aggregationFieldPath
        case 229:  // binary
        case 230:  // undefined
        case 231:  // objectId
        case 232:  // bool
        case 233:  // date
        case 234:  // null
        case 235:  // regex
        case 236:  // simpleValue
        case 237:  // compoundValue
        case 238:  // valueArray
        case 239:  // valueObject
        case 240:  // valueFields
        case 241:  // variable
        case 242:  // typeArray
        case 243:  // typeValue
        case 244:  // pipeline
        case 245:  // stageList
        case 246:  // stage
        case 247:  // inhibitOptimization
        case 248:  // unionWith
        case 249:  // skip
        case 250:  // limit
        case 251:  // project
        case 252:  // sample
        case 253:  // aggregationProjectFields
        case 254:  // aggregationProjectionObjectFields
        case 255:  // topLevelAggregationProjection
        case 256:  // aggregationProjection
        case 257:  // projectionCommon
        case 258:  // aggregationProjectionObject
        case 259:  // num
        case 260:  // expression
        case 261:  // exprFixedTwoArg
        case 262:  // exprFixedThreeArg
        case 263:  // slice
        case 264:  // expressionArray
        case 265:  // expressionObject
        case 266:  // expressionFields
        case 267:  // maths
        case 268:  // meta
        case 269:  // add
        case 270:  // boolExprs
        case 271:  // and
        case 272:  // or
        case 273:  // not
        case 274:  // literalEscapes
        case 275:  // const
        case 276:  // literal
        case 277:  // stringExps
        case 278:  // concat
        case 279:  // dateFromString
        case 280:  // dateToString
        case 281:  // indexOfBytes
        case 282:  // indexOfCP
        case 283:  // ltrim
        case 284:  // regexFind
        case 285:  // regexFindAll
        case 286:  // regexMatch
        case 287:  // regexArgs
        case 288:  // replaceOne
        case 289:  // replaceAll
        case 290:  // rtrim
        case 291:  // split
        case 292:  // strLenBytes
        case 293:  // strLenCP
        case 294:  // strcasecmp
        case 295:  // substr
        case 296:  // substrBytes
        case 297:  // substrCP
        case 298:  // toLower
        case 299:  // toUpper
        case 300:  // trim
        case 301:  // compExprs
        case 302:  // cmp
        case 303:  // eq
        case 304:  // gt
        case 305:  // gte
        case 306:  // lt
        case 307:  // lte
        case 308:  // ne
        case 309:  // dateExps
        case 310:  // dateFromParts
        case 311:  // dateToParts
        case 312:  // dayOfMonth
        case 313:  // dayOfWeek
        case 314:  // dayOfYear
        case 315:  // hour
        case 316:  // isoDayOfWeek
        case 317:  // isoWeek
        case 318:  // isoWeekYear
        case 319:  // millisecond
        case 320:  // minute
        case 321:  // month
        case 322:  // second
        case 323:  // week
        case 324:  // year
        case 325:  // typeExpression
        case 326:  // convert
        case 327:  // toBool
        case 328:  // toDate
        case 329:  // toDecimal
        case 330:  // toDouble
        case 331:  // toInt
        case 332:  // toLong
        case 333:  // toObjectId
        case 334:  // toString
        case 335:  // type
        case 336:  // abs
        case 337:  // ceil
        case 338:  // divide
        case 339:  // exponent
        case 340:  // floor
        case 341:  // ln
        case 342:  // log
        case 343:  // logten
        case 344:  // mod
        case 345:  // multiply
        case 346:  // pow
        case 347:  // round
        case 348:  // sqrt
        case 349:  // subtract
        case 350:  // trunc
        case 369:  // setExpression
        case 370:  // allElementsTrue
        case 371:  // anyElementTrue
        case 372:  // setDifference
        case 373:  // setEquals
        case 374:  // setIntersection
        case 375:  // setIsSubset
        case 376:  // setUnion
        case 377:  // trig
        case 378:  // sin
        case 379:  // cos
        case 380:  // tan
        case 381:  // sinh
        case 382:  // cosh
        case 383:  // tanh
        case 384:  // asin
        case 385:  // acos
        case 386:  // atan
        case 387:  // asinh
        case 388:  // acosh
        case 389:  // atanh
        case 390:  // atan2
        case 391:  // degreesToRadians
        case 392:  // radiansToDegrees
        case 393:  // nonArrayExpression
        case 394:  // nonArrayCompoundExpression
        case 395:  // aggregationOperator
        case 396:  // aggregationOperatorWithoutSlice
        case 397:  // expressionSingletonArray
        case 398:  // singleArgExpression
        case 399:  // nonArrayNonObjExpression
        case 400:  // match
        case 401:  // predicates
        case 402:  // compoundMatchExprs
        case 403:  // predValue
        case 404:  // additionalExprs
        case 414:  // findProject
        case 415:  // findProjectFields
        case 416:  // topLevelFindProjection
        case 417:  // findProjection
        case 418:  // findProjectionSlice
        case 419:  // elemMatch
        case 420:  // findProjectionObject
        case 421:  // findProjectionObjectFields
        case 424:  // sortSpecs
        case 425:  // specList
        case 426:  // metaSort
        case 427:  // oneOrNegOne
        case 428:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 196:  // aggregationProjectionFieldname
        case 197:  // projectionFieldname
        case 198:  // expressionFieldname
        case 199:  // stageAsUserFieldname
        case 200:  // argAsUserFieldname
        case 201:  // argAsProjectionPath
        case 202:  // aggExprAsUserFieldname
        case 203:  // invariableUserFieldname
        case 204:  // sortFieldname
        case 205:  // idAsUserFieldname
        case 206:  // elemMatchAsUserFieldname
        case 207:  // idAsProjectionPath
        case 208:  // valueFieldname
        case 209:  // predFieldname
        case 412:  // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 177:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 187:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 176:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 188:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 190:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 189:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 178:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 175:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 186:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 184:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 185:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 210:  // aggregationProjectField
        case 211:  // aggregationProjectionObjectField
        case 212:  // expressionField
        case 213:  // valueField
        case 351:  // onErrorArg
        case 352:  // onNullArg
        case 353:  // formatArg
        case 354:  // timezoneArg
        case 355:  // charsArg
        case 356:  // optionsArg
        case 357:  // hourArg
        case 358:  // minuteArg
        case 359:  // secondArg
        case 360:  // millisecondArg
        case 361:  // dayArg
        case 362:  // isoWeekArg
        case 363:  // iso8601Arg
        case 364:  // monthArg
        case 365:  // isoDayOfWeekArg
        case 405:  // predicate
        case 406:  // logicalExpr
        case 407:  // operatorExpression
        case 408:  // notExpr
        case 409:  // existsExpr
        case 410:  // typeExpr
        case 411:  // commentExpr
        case 422:  // findProjectField
        case 423:  // findProjectionObjectField
        case 429:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 168:  // "fieldname"
        case 170:  // "$-prefixed fieldname"
        case 171:  // "string"
        case 172:  // "$-prefixed string"
        case 173:  // "$$-prefixed string"
        case 214:  // arg
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 366:  // expressions
        case 367:  // values
        case 368:  // exprZeroToTwo
        case 413:  // typeValues
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 169:  // "fieldname containing dotted path"
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
        case 174:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 181:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 183:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 180:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 179:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 182:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 215:  // dbPointer
        case 216:  // javascript
        case 217:  // symbol
        case 218:  // javascriptWScope
        case 219:  // int
        case 220:  // timestamp
        case 221:  // long
        case 222:  // double
        case 223:  // decimal
        case 224:  // minKey
        case 225:  // maxKey
        case 226:  // value
        case 227:  // string
        case 228:  // aggregationFieldPath
        case 229:  // binary
        case 230:  // undefined
        case 231:  // objectId
        case 232:  // bool
        case 233:  // date
        case 234:  // null
        case 235:  // regex
        case 236:  // simpleValue
        case 237:  // compoundValue
        case 238:  // valueArray
        case 239:  // valueObject
        case 240:  // valueFields
        case 241:  // variable
        case 242:  // typeArray
        case 243:  // typeValue
        case 244:  // pipeline
        case 245:  // stageList
        case 246:  // stage
        case 247:  // inhibitOptimization
        case 248:  // unionWith
        case 249:  // skip
        case 250:  // limit
        case 251:  // project
        case 252:  // sample
        case 253:  // aggregationProjectFields
        case 254:  // aggregationProjectionObjectFields
        case 255:  // topLevelAggregationProjection
        case 256:  // aggregationProjection
        case 257:  // projectionCommon
        case 258:  // aggregationProjectionObject
        case 259:  // num
        case 260:  // expression
        case 261:  // exprFixedTwoArg
        case 262:  // exprFixedThreeArg
        case 263:  // slice
        case 264:  // expressionArray
        case 265:  // expressionObject
        case 266:  // expressionFields
        case 267:  // maths
        case 268:  // meta
        case 269:  // add
        case 270:  // boolExprs
        case 271:  // and
        case 272:  // or
        case 273:  // not
        case 274:  // literalEscapes
        case 275:  // const
        case 276:  // literal
        case 277:  // stringExps
        case 278:  // concat
        case 279:  // dateFromString
        case 280:  // dateToString
        case 281:  // indexOfBytes
        case 282:  // indexOfCP
        case 283:  // ltrim
        case 284:  // regexFind
        case 285:  // regexFindAll
        case 286:  // regexMatch
        case 287:  // regexArgs
        case 288:  // replaceOne
        case 289:  // replaceAll
        case 290:  // rtrim
        case 291:  // split
        case 292:  // strLenBytes
        case 293:  // strLenCP
        case 294:  // strcasecmp
        case 295:  // substr
        case 296:  // substrBytes
        case 297:  // substrCP
        case 298:  // toLower
        case 299:  // toUpper
        case 300:  // trim
        case 301:  // compExprs
        case 302:  // cmp
        case 303:  // eq
        case 304:  // gt
        case 305:  // gte
        case 306:  // lt
        case 307:  // lte
        case 308:  // ne
        case 309:  // dateExps
        case 310:  // dateFromParts
        case 311:  // dateToParts
        case 312:  // dayOfMonth
        case 313:  // dayOfWeek
        case 314:  // dayOfYear
        case 315:  // hour
        case 316:  // isoDayOfWeek
        case 317:  // isoWeek
        case 318:  // isoWeekYear
        case 319:  // millisecond
        case 320:  // minute
        case 321:  // month
        case 322:  // second
        case 323:  // week
        case 324:  // year
        case 325:  // typeExpression
        case 326:  // convert
        case 327:  // toBool
        case 328:  // toDate
        case 329:  // toDecimal
        case 330:  // toDouble
        case 331:  // toInt
        case 332:  // toLong
        case 333:  // toObjectId
        case 334:  // toString
        case 335:  // type
        case 336:  // abs
        case 337:  // ceil
        case 338:  // divide
        case 339:  // exponent
        case 340:  // floor
        case 341:  // ln
        case 342:  // log
        case 343:  // logten
        case 344:  // mod
        case 345:  // multiply
        case 346:  // pow
        case 347:  // round
        case 348:  // sqrt
        case 349:  // subtract
        case 350:  // trunc
        case 369:  // setExpression
        case 370:  // allElementsTrue
        case 371:  // anyElementTrue
        case 372:  // setDifference
        case 373:  // setEquals
        case 374:  // setIntersection
        case 375:  // setIsSubset
        case 376:  // setUnion
        case 377:  // trig
        case 378:  // sin
        case 379:  // cos
        case 380:  // tan
        case 381:  // sinh
        case 382:  // cosh
        case 383:  // tanh
        case 384:  // asin
        case 385:  // acos
        case 386:  // atan
        case 387:  // asinh
        case 388:  // acosh
        case 389:  // atanh
        case 390:  // atan2
        case 391:  // degreesToRadians
        case 392:  // radiansToDegrees
        case 393:  // nonArrayExpression
        case 394:  // nonArrayCompoundExpression
        case 395:  // aggregationOperator
        case 396:  // aggregationOperatorWithoutSlice
        case 397:  // expressionSingletonArray
        case 398:  // singleArgExpression
        case 399:  // nonArrayNonObjExpression
        case 400:  // match
        case 401:  // predicates
        case 402:  // compoundMatchExprs
        case 403:  // predValue
        case 404:  // additionalExprs
        case 414:  // findProject
        case 415:  // findProjectFields
        case 416:  // topLevelFindProjection
        case 417:  // findProjection
        case 418:  // findProjectionSlice
        case 419:  // elemMatch
        case 420:  // findProjectionObject
        case 421:  // findProjectionObjectFields
        case 424:  // sortSpecs
        case 425:  // specList
        case 426:  // metaSort
        case 427:  // oneOrNegOne
        case 428:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 196:  // aggregationProjectionFieldname
        case 197:  // projectionFieldname
        case 198:  // expressionFieldname
        case 199:  // stageAsUserFieldname
        case 200:  // argAsUserFieldname
        case 201:  // argAsProjectionPath
        case 202:  // aggExprAsUserFieldname
        case 203:  // invariableUserFieldname
        case 204:  // sortFieldname
        case 205:  // idAsUserFieldname
        case 206:  // elemMatchAsUserFieldname
        case 207:  // idAsProjectionPath
        case 208:  // valueFieldname
        case 209:  // predFieldname
        case 412:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 177:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 187:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 176:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 188:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 190:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 189:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 178:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 175:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 186:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 184:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 185:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 210:  // aggregationProjectField
        case 211:  // aggregationProjectionObjectField
        case 212:  // expressionField
        case 213:  // valueField
        case 351:  // onErrorArg
        case 352:  // onNullArg
        case 353:  // formatArg
        case 354:  // timezoneArg
        case 355:  // charsArg
        case 356:  // optionsArg
        case 357:  // hourArg
        case 358:  // minuteArg
        case 359:  // secondArg
        case 360:  // millisecondArg
        case 361:  // dayArg
        case 362:  // isoWeekArg
        case 363:  // iso8601Arg
        case 364:  // monthArg
        case 365:  // isoDayOfWeekArg
        case 405:  // predicate
        case 406:  // logicalExpr
        case 407:  // operatorExpression
        case 408:  // notExpr
        case 409:  // existsExpr
        case 410:  // typeExpr
        case 411:  // commentExpr
        case 422:  // findProjectField
        case 423:  // findProjectionObjectField
        case 429:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 168:  // "fieldname"
        case 170:  // "$-prefixed fieldname"
        case 171:  // "string"
        case 172:  // "$-prefixed string"
        case 173:  // "$$-prefixed string"
        case 214:  // arg
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 366:  // expressions
        case 367:  // values
        case 368:  // exprZeroToTwo
        case 413:  // typeValues
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 169:  // "fieldname containing dotted path"
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
        case 174:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 181:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 183:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 180:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 179:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 182:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 215:  // dbPointer
        case 216:  // javascript
        case 217:  // symbol
        case 218:  // javascriptWScope
        case 219:  // int
        case 220:  // timestamp
        case 221:  // long
        case 222:  // double
        case 223:  // decimal
        case 224:  // minKey
        case 225:  // maxKey
        case 226:  // value
        case 227:  // string
        case 228:  // aggregationFieldPath
        case 229:  // binary
        case 230:  // undefined
        case 231:  // objectId
        case 232:  // bool
        case 233:  // date
        case 234:  // null
        case 235:  // regex
        case 236:  // simpleValue
        case 237:  // compoundValue
        case 238:  // valueArray
        case 239:  // valueObject
        case 240:  // valueFields
        case 241:  // variable
        case 242:  // typeArray
        case 243:  // typeValue
        case 244:  // pipeline
        case 245:  // stageList
        case 246:  // stage
        case 247:  // inhibitOptimization
        case 248:  // unionWith
        case 249:  // skip
        case 250:  // limit
        case 251:  // project
        case 252:  // sample
        case 253:  // aggregationProjectFields
        case 254:  // aggregationProjectionObjectFields
        case 255:  // topLevelAggregationProjection
        case 256:  // aggregationProjection
        case 257:  // projectionCommon
        case 258:  // aggregationProjectionObject
        case 259:  // num
        case 260:  // expression
        case 261:  // exprFixedTwoArg
        case 262:  // exprFixedThreeArg
        case 263:  // slice
        case 264:  // expressionArray
        case 265:  // expressionObject
        case 266:  // expressionFields
        case 267:  // maths
        case 268:  // meta
        case 269:  // add
        case 270:  // boolExprs
        case 271:  // and
        case 272:  // or
        case 273:  // not
        case 274:  // literalEscapes
        case 275:  // const
        case 276:  // literal
        case 277:  // stringExps
        case 278:  // concat
        case 279:  // dateFromString
        case 280:  // dateToString
        case 281:  // indexOfBytes
        case 282:  // indexOfCP
        case 283:  // ltrim
        case 284:  // regexFind
        case 285:  // regexFindAll
        case 286:  // regexMatch
        case 287:  // regexArgs
        case 288:  // replaceOne
        case 289:  // replaceAll
        case 290:  // rtrim
        case 291:  // split
        case 292:  // strLenBytes
        case 293:  // strLenCP
        case 294:  // strcasecmp
        case 295:  // substr
        case 296:  // substrBytes
        case 297:  // substrCP
        case 298:  // toLower
        case 299:  // toUpper
        case 300:  // trim
        case 301:  // compExprs
        case 302:  // cmp
        case 303:  // eq
        case 304:  // gt
        case 305:  // gte
        case 306:  // lt
        case 307:  // lte
        case 308:  // ne
        case 309:  // dateExps
        case 310:  // dateFromParts
        case 311:  // dateToParts
        case 312:  // dayOfMonth
        case 313:  // dayOfWeek
        case 314:  // dayOfYear
        case 315:  // hour
        case 316:  // isoDayOfWeek
        case 317:  // isoWeek
        case 318:  // isoWeekYear
        case 319:  // millisecond
        case 320:  // minute
        case 321:  // month
        case 322:  // second
        case 323:  // week
        case 324:  // year
        case 325:  // typeExpression
        case 326:  // convert
        case 327:  // toBool
        case 328:  // toDate
        case 329:  // toDecimal
        case 330:  // toDouble
        case 331:  // toInt
        case 332:  // toLong
        case 333:  // toObjectId
        case 334:  // toString
        case 335:  // type
        case 336:  // abs
        case 337:  // ceil
        case 338:  // divide
        case 339:  // exponent
        case 340:  // floor
        case 341:  // ln
        case 342:  // log
        case 343:  // logten
        case 344:  // mod
        case 345:  // multiply
        case 346:  // pow
        case 347:  // round
        case 348:  // sqrt
        case 349:  // subtract
        case 350:  // trunc
        case 369:  // setExpression
        case 370:  // allElementsTrue
        case 371:  // anyElementTrue
        case 372:  // setDifference
        case 373:  // setEquals
        case 374:  // setIntersection
        case 375:  // setIsSubset
        case 376:  // setUnion
        case 377:  // trig
        case 378:  // sin
        case 379:  // cos
        case 380:  // tan
        case 381:  // sinh
        case 382:  // cosh
        case 383:  // tanh
        case 384:  // asin
        case 385:  // acos
        case 386:  // atan
        case 387:  // asinh
        case 388:  // acosh
        case 389:  // atanh
        case 390:  // atan2
        case 391:  // degreesToRadians
        case 392:  // radiansToDegrees
        case 393:  // nonArrayExpression
        case 394:  // nonArrayCompoundExpression
        case 395:  // aggregationOperator
        case 396:  // aggregationOperatorWithoutSlice
        case 397:  // expressionSingletonArray
        case 398:  // singleArgExpression
        case 399:  // nonArrayNonObjExpression
        case 400:  // match
        case 401:  // predicates
        case 402:  // compoundMatchExprs
        case 403:  // predValue
        case 404:  // additionalExprs
        case 414:  // findProject
        case 415:  // findProjectFields
        case 416:  // topLevelFindProjection
        case 417:  // findProjection
        case 418:  // findProjectionSlice
        case 419:  // elemMatch
        case 420:  // findProjectionObject
        case 421:  // findProjectionObjectFields
        case 424:  // sortSpecs
        case 425:  // specList
        case 426:  // metaSort
        case 427:  // oneOrNegOne
        case 428:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 196:  // aggregationProjectionFieldname
        case 197:  // projectionFieldname
        case 198:  // expressionFieldname
        case 199:  // stageAsUserFieldname
        case 200:  // argAsUserFieldname
        case 201:  // argAsProjectionPath
        case 202:  // aggExprAsUserFieldname
        case 203:  // invariableUserFieldname
        case 204:  // sortFieldname
        case 205:  // idAsUserFieldname
        case 206:  // elemMatchAsUserFieldname
        case 207:  // idAsProjectionPath
        case 208:  // valueFieldname
        case 209:  // predFieldname
        case 412:  // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 177:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 187:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 176:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 188:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 190:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 189:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 178:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 175:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 186:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 184:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 185:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 210:  // aggregationProjectField
        case 211:  // aggregationProjectionObjectField
        case 212:  // expressionField
        case 213:  // valueField
        case 351:  // onErrorArg
        case 352:  // onNullArg
        case 353:  // formatArg
        case 354:  // timezoneArg
        case 355:  // charsArg
        case 356:  // optionsArg
        case 357:  // hourArg
        case 358:  // minuteArg
        case 359:  // secondArg
        case 360:  // millisecondArg
        case 361:  // dayArg
        case 362:  // isoWeekArg
        case 363:  // iso8601Arg
        case 364:  // monthArg
        case 365:  // isoDayOfWeekArg
        case 405:  // predicate
        case 406:  // logicalExpr
        case 407:  // operatorExpression
        case 408:  // notExpr
        case 409:  // existsExpr
        case 410:  // typeExpr
        case 411:  // commentExpr
        case 422:  // findProjectField
        case 423:  // findProjectionObjectField
        case 429:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 168:  // "fieldname"
        case 170:  // "$-prefixed fieldname"
        case 171:  // "string"
        case 172:  // "$-prefixed string"
        case 173:  // "$$-prefixed string"
        case 214:  // arg
            value.copy<std::string>(that.value);
            break;

        case 366:  // expressions
        case 367:  // values
        case 368:  // exprZeroToTwo
        case 413:  // typeValues
            value.copy<std::vector<CNode>>(that.value);
            break;

        case 169:  // "fieldname containing dotted path"
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
        case 174:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 181:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 183:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 180:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 179:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 182:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 215:  // dbPointer
        case 216:  // javascript
        case 217:  // symbol
        case 218:  // javascriptWScope
        case 219:  // int
        case 220:  // timestamp
        case 221:  // long
        case 222:  // double
        case 223:  // decimal
        case 224:  // minKey
        case 225:  // maxKey
        case 226:  // value
        case 227:  // string
        case 228:  // aggregationFieldPath
        case 229:  // binary
        case 230:  // undefined
        case 231:  // objectId
        case 232:  // bool
        case 233:  // date
        case 234:  // null
        case 235:  // regex
        case 236:  // simpleValue
        case 237:  // compoundValue
        case 238:  // valueArray
        case 239:  // valueObject
        case 240:  // valueFields
        case 241:  // variable
        case 242:  // typeArray
        case 243:  // typeValue
        case 244:  // pipeline
        case 245:  // stageList
        case 246:  // stage
        case 247:  // inhibitOptimization
        case 248:  // unionWith
        case 249:  // skip
        case 250:  // limit
        case 251:  // project
        case 252:  // sample
        case 253:  // aggregationProjectFields
        case 254:  // aggregationProjectionObjectFields
        case 255:  // topLevelAggregationProjection
        case 256:  // aggregationProjection
        case 257:  // projectionCommon
        case 258:  // aggregationProjectionObject
        case 259:  // num
        case 260:  // expression
        case 261:  // exprFixedTwoArg
        case 262:  // exprFixedThreeArg
        case 263:  // slice
        case 264:  // expressionArray
        case 265:  // expressionObject
        case 266:  // expressionFields
        case 267:  // maths
        case 268:  // meta
        case 269:  // add
        case 270:  // boolExprs
        case 271:  // and
        case 272:  // or
        case 273:  // not
        case 274:  // literalEscapes
        case 275:  // const
        case 276:  // literal
        case 277:  // stringExps
        case 278:  // concat
        case 279:  // dateFromString
        case 280:  // dateToString
        case 281:  // indexOfBytes
        case 282:  // indexOfCP
        case 283:  // ltrim
        case 284:  // regexFind
        case 285:  // regexFindAll
        case 286:  // regexMatch
        case 287:  // regexArgs
        case 288:  // replaceOne
        case 289:  // replaceAll
        case 290:  // rtrim
        case 291:  // split
        case 292:  // strLenBytes
        case 293:  // strLenCP
        case 294:  // strcasecmp
        case 295:  // substr
        case 296:  // substrBytes
        case 297:  // substrCP
        case 298:  // toLower
        case 299:  // toUpper
        case 300:  // trim
        case 301:  // compExprs
        case 302:  // cmp
        case 303:  // eq
        case 304:  // gt
        case 305:  // gte
        case 306:  // lt
        case 307:  // lte
        case 308:  // ne
        case 309:  // dateExps
        case 310:  // dateFromParts
        case 311:  // dateToParts
        case 312:  // dayOfMonth
        case 313:  // dayOfWeek
        case 314:  // dayOfYear
        case 315:  // hour
        case 316:  // isoDayOfWeek
        case 317:  // isoWeek
        case 318:  // isoWeekYear
        case 319:  // millisecond
        case 320:  // minute
        case 321:  // month
        case 322:  // second
        case 323:  // week
        case 324:  // year
        case 325:  // typeExpression
        case 326:  // convert
        case 327:  // toBool
        case 328:  // toDate
        case 329:  // toDecimal
        case 330:  // toDouble
        case 331:  // toInt
        case 332:  // toLong
        case 333:  // toObjectId
        case 334:  // toString
        case 335:  // type
        case 336:  // abs
        case 337:  // ceil
        case 338:  // divide
        case 339:  // exponent
        case 340:  // floor
        case 341:  // ln
        case 342:  // log
        case 343:  // logten
        case 344:  // mod
        case 345:  // multiply
        case 346:  // pow
        case 347:  // round
        case 348:  // sqrt
        case 349:  // subtract
        case 350:  // trunc
        case 369:  // setExpression
        case 370:  // allElementsTrue
        case 371:  // anyElementTrue
        case 372:  // setDifference
        case 373:  // setEquals
        case 374:  // setIntersection
        case 375:  // setIsSubset
        case 376:  // setUnion
        case 377:  // trig
        case 378:  // sin
        case 379:  // cos
        case 380:  // tan
        case 381:  // sinh
        case 382:  // cosh
        case 383:  // tanh
        case 384:  // asin
        case 385:  // acos
        case 386:  // atan
        case 387:  // asinh
        case 388:  // acosh
        case 389:  // atanh
        case 390:  // atan2
        case 391:  // degreesToRadians
        case 392:  // radiansToDegrees
        case 393:  // nonArrayExpression
        case 394:  // nonArrayCompoundExpression
        case 395:  // aggregationOperator
        case 396:  // aggregationOperatorWithoutSlice
        case 397:  // expressionSingletonArray
        case 398:  // singleArgExpression
        case 399:  // nonArrayNonObjExpression
        case 400:  // match
        case 401:  // predicates
        case 402:  // compoundMatchExprs
        case 403:  // predValue
        case 404:  // additionalExprs
        case 414:  // findProject
        case 415:  // findProjectFields
        case 416:  // topLevelFindProjection
        case 417:  // findProjection
        case 418:  // findProjectionSlice
        case 419:  // elemMatch
        case 420:  // findProjectionObject
        case 421:  // findProjectionObjectFields
        case 424:  // sortSpecs
        case 425:  // specList
        case 426:  // metaSort
        case 427:  // oneOrNegOne
        case 428:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 196:  // aggregationProjectionFieldname
        case 197:  // projectionFieldname
        case 198:  // expressionFieldname
        case 199:  // stageAsUserFieldname
        case 200:  // argAsUserFieldname
        case 201:  // argAsProjectionPath
        case 202:  // aggExprAsUserFieldname
        case 203:  // invariableUserFieldname
        case 204:  // sortFieldname
        case 205:  // idAsUserFieldname
        case 206:  // elemMatchAsUserFieldname
        case 207:  // idAsProjectionPath
        case 208:  // valueFieldname
        case 209:  // predFieldname
        case 412:  // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case 177:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 187:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 176:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 188:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 190:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 189:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 178:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 175:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 186:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 184:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 185:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 210:  // aggregationProjectField
        case 211:  // aggregationProjectionObjectField
        case 212:  // expressionField
        case 213:  // valueField
        case 351:  // onErrorArg
        case 352:  // onNullArg
        case 353:  // formatArg
        case 354:  // timezoneArg
        case 355:  // charsArg
        case 356:  // optionsArg
        case 357:  // hourArg
        case 358:  // minuteArg
        case 359:  // secondArg
        case 360:  // millisecondArg
        case 361:  // dayArg
        case 362:  // isoWeekArg
        case 363:  // iso8601Arg
        case 364:  // monthArg
        case 365:  // isoDayOfWeekArg
        case 405:  // predicate
        case 406:  // logicalExpr
        case 407:  // operatorExpression
        case 408:  // notExpr
        case 409:  // existsExpr
        case 410:  // typeExpr
        case 411:  // commentExpr
        case 422:  // findProjectField
        case 423:  // findProjectionObjectField
        case 429:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 168:  // "fieldname"
        case 170:  // "$-prefixed fieldname"
        case 171:  // "string"
        case 172:  // "$-prefixed string"
        case 173:  // "$$-prefixed string"
        case 214:  // arg
            value.move<std::string>(that.value);
            break;

        case 366:  // expressions
        case 367:  // values
        case 368:  // exprZeroToTwo
        case 413:  // typeValues
            value.move<std::vector<CNode>>(that.value);
            break;

        case 169:  // "fieldname containing dotted path"
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
                case 174:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 181:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 183:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 180:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 179:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 182:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 215:  // dbPointer
                case 216:  // javascript
                case 217:  // symbol
                case 218:  // javascriptWScope
                case 219:  // int
                case 220:  // timestamp
                case 221:  // long
                case 222:  // double
                case 223:  // decimal
                case 224:  // minKey
                case 225:  // maxKey
                case 226:  // value
                case 227:  // string
                case 228:  // aggregationFieldPath
                case 229:  // binary
                case 230:  // undefined
                case 231:  // objectId
                case 232:  // bool
                case 233:  // date
                case 234:  // null
                case 235:  // regex
                case 236:  // simpleValue
                case 237:  // compoundValue
                case 238:  // valueArray
                case 239:  // valueObject
                case 240:  // valueFields
                case 241:  // variable
                case 242:  // typeArray
                case 243:  // typeValue
                case 244:  // pipeline
                case 245:  // stageList
                case 246:  // stage
                case 247:  // inhibitOptimization
                case 248:  // unionWith
                case 249:  // skip
                case 250:  // limit
                case 251:  // project
                case 252:  // sample
                case 253:  // aggregationProjectFields
                case 254:  // aggregationProjectionObjectFields
                case 255:  // topLevelAggregationProjection
                case 256:  // aggregationProjection
                case 257:  // projectionCommon
                case 258:  // aggregationProjectionObject
                case 259:  // num
                case 260:  // expression
                case 261:  // exprFixedTwoArg
                case 262:  // exprFixedThreeArg
                case 263:  // slice
                case 264:  // expressionArray
                case 265:  // expressionObject
                case 266:  // expressionFields
                case 267:  // maths
                case 268:  // meta
                case 269:  // add
                case 270:  // boolExprs
                case 271:  // and
                case 272:  // or
                case 273:  // not
                case 274:  // literalEscapes
                case 275:  // const
                case 276:  // literal
                case 277:  // stringExps
                case 278:  // concat
                case 279:  // dateFromString
                case 280:  // dateToString
                case 281:  // indexOfBytes
                case 282:  // indexOfCP
                case 283:  // ltrim
                case 284:  // regexFind
                case 285:  // regexFindAll
                case 286:  // regexMatch
                case 287:  // regexArgs
                case 288:  // replaceOne
                case 289:  // replaceAll
                case 290:  // rtrim
                case 291:  // split
                case 292:  // strLenBytes
                case 293:  // strLenCP
                case 294:  // strcasecmp
                case 295:  // substr
                case 296:  // substrBytes
                case 297:  // substrCP
                case 298:  // toLower
                case 299:  // toUpper
                case 300:  // trim
                case 301:  // compExprs
                case 302:  // cmp
                case 303:  // eq
                case 304:  // gt
                case 305:  // gte
                case 306:  // lt
                case 307:  // lte
                case 308:  // ne
                case 309:  // dateExps
                case 310:  // dateFromParts
                case 311:  // dateToParts
                case 312:  // dayOfMonth
                case 313:  // dayOfWeek
                case 314:  // dayOfYear
                case 315:  // hour
                case 316:  // isoDayOfWeek
                case 317:  // isoWeek
                case 318:  // isoWeekYear
                case 319:  // millisecond
                case 320:  // minute
                case 321:  // month
                case 322:  // second
                case 323:  // week
                case 324:  // year
                case 325:  // typeExpression
                case 326:  // convert
                case 327:  // toBool
                case 328:  // toDate
                case 329:  // toDecimal
                case 330:  // toDouble
                case 331:  // toInt
                case 332:  // toLong
                case 333:  // toObjectId
                case 334:  // toString
                case 335:  // type
                case 336:  // abs
                case 337:  // ceil
                case 338:  // divide
                case 339:  // exponent
                case 340:  // floor
                case 341:  // ln
                case 342:  // log
                case 343:  // logten
                case 344:  // mod
                case 345:  // multiply
                case 346:  // pow
                case 347:  // round
                case 348:  // sqrt
                case 349:  // subtract
                case 350:  // trunc
                case 369:  // setExpression
                case 370:  // allElementsTrue
                case 371:  // anyElementTrue
                case 372:  // setDifference
                case 373:  // setEquals
                case 374:  // setIntersection
                case 375:  // setIsSubset
                case 376:  // setUnion
                case 377:  // trig
                case 378:  // sin
                case 379:  // cos
                case 380:  // tan
                case 381:  // sinh
                case 382:  // cosh
                case 383:  // tanh
                case 384:  // asin
                case 385:  // acos
                case 386:  // atan
                case 387:  // asinh
                case 388:  // acosh
                case 389:  // atanh
                case 390:  // atan2
                case 391:  // degreesToRadians
                case 392:  // radiansToDegrees
                case 393:  // nonArrayExpression
                case 394:  // nonArrayCompoundExpression
                case 395:  // aggregationOperator
                case 396:  // aggregationOperatorWithoutSlice
                case 397:  // expressionSingletonArray
                case 398:  // singleArgExpression
                case 399:  // nonArrayNonObjExpression
                case 400:  // match
                case 401:  // predicates
                case 402:  // compoundMatchExprs
                case 403:  // predValue
                case 404:  // additionalExprs
                case 414:  // findProject
                case 415:  // findProjectFields
                case 416:  // topLevelFindProjection
                case 417:  // findProjection
                case 418:  // findProjectionSlice
                case 419:  // elemMatch
                case 420:  // findProjectionObject
                case 421:  // findProjectionObjectFields
                case 424:  // sortSpecs
                case 425:  // specList
                case 426:  // metaSort
                case 427:  // oneOrNegOne
                case 428:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 196:  // aggregationProjectionFieldname
                case 197:  // projectionFieldname
                case 198:  // expressionFieldname
                case 199:  // stageAsUserFieldname
                case 200:  // argAsUserFieldname
                case 201:  // argAsProjectionPath
                case 202:  // aggExprAsUserFieldname
                case 203:  // invariableUserFieldname
                case 204:  // sortFieldname
                case 205:  // idAsUserFieldname
                case 206:  // elemMatchAsUserFieldname
                case 207:  // idAsProjectionPath
                case 208:  // valueFieldname
                case 209:  // predFieldname
                case 412:  // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 177:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 187:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 176:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 188:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 190:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 189:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 178:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 175:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 186:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 184:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 185:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 210:  // aggregationProjectField
                case 211:  // aggregationProjectionObjectField
                case 212:  // expressionField
                case 213:  // valueField
                case 351:  // onErrorArg
                case 352:  // onNullArg
                case 353:  // formatArg
                case 354:  // timezoneArg
                case 355:  // charsArg
                case 356:  // optionsArg
                case 357:  // hourArg
                case 358:  // minuteArg
                case 359:  // secondArg
                case 360:  // millisecondArg
                case 361:  // dayArg
                case 362:  // isoWeekArg
                case 363:  // iso8601Arg
                case 364:  // monthArg
                case 365:  // isoDayOfWeekArg
                case 405:  // predicate
                case 406:  // logicalExpr
                case 407:  // operatorExpression
                case 408:  // notExpr
                case 409:  // existsExpr
                case 410:  // typeExpr
                case 411:  // commentExpr
                case 422:  // findProjectField
                case 423:  // findProjectionObjectField
                case 429:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 168:  // "fieldname"
                case 170:  // "$-prefixed fieldname"
                case 171:  // "string"
                case 172:  // "$-prefixed string"
                case 173:  // "$$-prefixed string"
                case 214:  // arg
                    yylhs.value.emplace<std::string>();
                    break;

                case 366:  // expressions
                case 367:  // values
                case 368:  // exprZeroToTwo
                case 413:  // typeValues
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                case 169:  // "fieldname containing dotted path"
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
#line 393 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2193 "parser_gen.cpp"
                    break;

                    case 3:
#line 396 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2201 "parser_gen.cpp"
                    break;

                    case 4:
#line 399 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2209 "parser_gen.cpp"
                    break;

                    case 5:
#line 402 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2217 "parser_gen.cpp"
                    break;

                    case 6:
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2225 "parser_gen.cpp"
                    break;

                    case 7:
#line 415 "grammar.yy"
                    {
                    }
#line 2231 "parser_gen.cpp"
                    break;

                    case 8:
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2239 "parser_gen.cpp"
                    break;

                    case 9:
#line 424 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2245 "parser_gen.cpp"
                    break;

                    case 10:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2251 "parser_gen.cpp"
                    break;

                    case 11:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2257 "parser_gen.cpp"
                    break;

                    case 12:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2263 "parser_gen.cpp"
                    break;

                    case 13:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2269 "parser_gen.cpp"
                    break;

                    case 14:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2275 "parser_gen.cpp"
                    break;

                    case 15:
#line 427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2281 "parser_gen.cpp"
                    break;

                    case 16:
#line 430 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2293 "parser_gen.cpp"
                    break;

                    case 17:
#line 440 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2301 "parser_gen.cpp"
                    break;

                    case 18:
#line 446 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2314 "parser_gen.cpp"
                    break;

                    case 19:
#line 456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2320 "parser_gen.cpp"
                    break;

                    case 20:
#line 456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2326 "parser_gen.cpp"
                    break;

                    case 21:
#line 456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2332 "parser_gen.cpp"
                    break;

                    case 22:
#line 456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2338 "parser_gen.cpp"
                    break;

                    case 23:
#line 460 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2346 "parser_gen.cpp"
                    break;

                    case 24:
#line 465 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2354 "parser_gen.cpp"
                    break;

                    case 25:
#line 470 "grammar.yy"
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
#line 2375 "parser_gen.cpp"
                    break;

                    case 26:
#line 489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2383 "parser_gen.cpp"
                    break;

                    case 27:
#line 492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2392 "parser_gen.cpp"
                    break;

                    case 28:
#line 499 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2400 "parser_gen.cpp"
                    break;

                    case 29:
#line 502 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2408 "parser_gen.cpp"
                    break;

                    case 30:
#line 508 "grammar.yy"
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
#line 2424 "parser_gen.cpp"
                    break;

                    case 31:
#line 522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2430 "parser_gen.cpp"
                    break;

                    case 32:
#line 523 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2436 "parser_gen.cpp"
                    break;

                    case 33:
#line 524 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2442 "parser_gen.cpp"
                    break;

                    case 34:
#line 528 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2448 "parser_gen.cpp"
                    break;

                    case 35:
#line 529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2454 "parser_gen.cpp"
                    break;

                    case 36:
#line 530 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2460 "parser_gen.cpp"
                    break;

                    case 37:
#line 531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2466 "parser_gen.cpp"
                    break;

                    case 38:
#line 532 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2472 "parser_gen.cpp"
                    break;

                    case 39:
#line 533 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2478 "parser_gen.cpp"
                    break;

                    case 40:
#line 534 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2484 "parser_gen.cpp"
                    break;

                    case 41:
#line 535 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2490 "parser_gen.cpp"
                    break;

                    case 42:
#line 536 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2496 "parser_gen.cpp"
                    break;

                    case 43:
#line 537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2502 "parser_gen.cpp"
                    break;

                    case 44:
#line 538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2508 "parser_gen.cpp"
                    break;

                    case 45:
#line 539 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2516 "parser_gen.cpp"
                    break;

                    case 46:
#line 542 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2524 "parser_gen.cpp"
                    break;

                    case 47:
#line 545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2532 "parser_gen.cpp"
                    break;

                    case 48:
#line 548 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2540 "parser_gen.cpp"
                    break;

                    case 49:
#line 551 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2548 "parser_gen.cpp"
                    break;

                    case 50:
#line 554 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2556 "parser_gen.cpp"
                    break;

                    case 51:
#line 557 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2564 "parser_gen.cpp"
                    break;

                    case 52:
#line 560 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2572 "parser_gen.cpp"
                    break;

                    case 53:
#line 563 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2580 "parser_gen.cpp"
                    break;

                    case 54:
#line 566 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2588 "parser_gen.cpp"
                    break;

                    case 55:
#line 569 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2596 "parser_gen.cpp"
                    break;

                    case 56:
#line 572 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2604 "parser_gen.cpp"
                    break;

                    case 57:
#line 575 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2612 "parser_gen.cpp"
                    break;

                    case 58:
#line 578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2620 "parser_gen.cpp"
                    break;

                    case 59:
#line 581 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2628 "parser_gen.cpp"
                    break;

                    case 60:
#line 584 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2636 "parser_gen.cpp"
                    break;

                    case 61:
#line 587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2644 "parser_gen.cpp"
                    break;

                    case 62:
#line 590 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2652 "parser_gen.cpp"
                    break;

                    case 63:
#line 593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2658 "parser_gen.cpp"
                    break;

                    case 64:
#line 594 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2664 "parser_gen.cpp"
                    break;

                    case 65:
#line 595 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2670 "parser_gen.cpp"
                    break;

                    case 66:
#line 596 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2676 "parser_gen.cpp"
                    break;

                    case 67:
#line 601 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2686 "parser_gen.cpp"
                    break;

                    case 68:
#line 610 "grammar.yy"
                    {
                        auto components =
                            makeVector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK()) {
                            yylhs.value.as<CNode::Fieldname>() =
                                c_node_disambiguation::disambiguateProjectionPathType(
                                    std::move(components), positional.getValue());
                        } else {
                            error(yystack_[0].location, positional.getStatus().reason());
                        }
                    }
#line 2702 "parser_gen.cpp"
                    break;

                    case 69:
#line 621 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2708 "parser_gen.cpp"
                    break;

                    case 70:
#line 622 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK()) {
                            yylhs.value.as<CNode::Fieldname>() =
                                c_node_disambiguation::disambiguateProjectionPathType(
                                    std::move(components), positional.getValue());
                        } else {
                            error(yystack_[0].location, positional.getStatus().reason());
                        }
                    }
#line 2724 "parser_gen.cpp"
                    break;

                    case 71:
#line 637 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2732 "parser_gen.cpp"
                    break;

                    case 72:
#line 644 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2741 "parser_gen.cpp"
                    break;

                    case 73:
#line 648 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2750 "parser_gen.cpp"
                    break;

                    case 74:
#line 656 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2758 "parser_gen.cpp"
                    break;

                    case 75:
#line 659 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2766 "parser_gen.cpp"
                    break;

                    case 76:
#line 665 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2774 "parser_gen.cpp"
                    break;

                    case 77:
#line 671 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2782 "parser_gen.cpp"
                    break;

                    case 78:
#line 674 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2791 "parser_gen.cpp"
                    break;

                    case 79:
#line 680 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2799 "parser_gen.cpp"
                    break;

                    case 80:
#line 683 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2805 "parser_gen.cpp"
                    break;

                    case 81:
#line 684 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2811 "parser_gen.cpp"
                    break;

                    case 82:
#line 691 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2817 "parser_gen.cpp"
                    break;

                    case 83:
#line 692 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2825 "parser_gen.cpp"
                    break;

                    case 84:
#line 698 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2833 "parser_gen.cpp"
                    break;

                    case 85:
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2842 "parser_gen.cpp"
                    break;

                    case 86:
#line 709 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2848 "parser_gen.cpp"
                    break;

                    case 87:
#line 709 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2854 "parser_gen.cpp"
                    break;

                    case 88:
#line 709 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2860 "parser_gen.cpp"
                    break;

                    case 89:
#line 713 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2868 "parser_gen.cpp"
                    break;

                    case 90:
#line 719 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2876 "parser_gen.cpp"
                    break;

                    case 91:
#line 725 "grammar.yy"
                    {
                    }
#line 2882 "parser_gen.cpp"
                    break;

                    case 92:
#line 726 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2891 "parser_gen.cpp"
                    break;

                    case 93:
#line 733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2897 "parser_gen.cpp"
                    break;

                    case 94:
#line 733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2903 "parser_gen.cpp"
                    break;

                    case 95:
#line 737 "grammar.yy"
                    {
                        auto&& type = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(type);
                            !status.isOK()) {
                            // TODO SERVER-50498: error() on the offending literal rather than the
                            // TYPE token. This will require removing the offending literal
                            // indicators in the error strings provided by the validation function.
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(type)};
                    }
#line 2917 "parser_gen.cpp"
                    break;

                    case 96:
#line 746 "grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 2929 "parser_gen.cpp"
                    break;

                    case 97:
#line 756 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2937 "parser_gen.cpp"
                    break;

                    case 98:
#line 762 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2945 "parser_gen.cpp"
                    break;

                    case 99:
#line 767 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2956 "parser_gen.cpp"
                    break;

                    case 100:
#line 777 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2966 "parser_gen.cpp"
                    break;

                    case 101:
#line 785 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2972 "parser_gen.cpp"
                    break;

                    case 102:
#line 786 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2978 "parser_gen.cpp"
                    break;

                    case 103:
#line 787 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2984 "parser_gen.cpp"
                    break;

                    case 104:
#line 790 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2992 "parser_gen.cpp"
                    break;

                    case 105:
#line 793 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3001 "parser_gen.cpp"
                    break;

                    case 106:
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3007 "parser_gen.cpp"
                    break;

                    case 107:
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3013 "parser_gen.cpp"
                    break;

                    case 108:
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3019 "parser_gen.cpp"
                    break;

                    case 109:
#line 803 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3027 "parser_gen.cpp"
                    break;

                    case 110:
#line 811 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 3035 "parser_gen.cpp"
                    break;

                    case 111:
#line 814 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 3043 "parser_gen.cpp"
                    break;

                    case 112:
#line 817 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 3051 "parser_gen.cpp"
                    break;

                    case 113:
#line 820 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 3059 "parser_gen.cpp"
                    break;

                    case 114:
#line 823 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 3067 "parser_gen.cpp"
                    break;

                    case 115:
#line 826 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 3075 "parser_gen.cpp"
                    break;

                    case 116:
#line 832 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3083 "parser_gen.cpp"
                    break;

                    case 117:
#line 838 "grammar.yy"
                    {
                        auto components =
                            makeVector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
                        if (auto positional =
                                c_node_validation::validateProjectionPathAsNormalOrPositional(
                                    components);
                            positional.isOK())
                            yylhs.value.as<CNode::Fieldname>() =
                                c_node_disambiguation::disambiguateProjectionPathType(
                                    std::move(components), positional.getValue());
                        else
                            error(yystack_[0].location, positional.getStatus().reason());
                    }
#line 3098 "parser_gen.cpp"
                    break;

                    case 118:
#line 854 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 3106 "parser_gen.cpp"
                    break;

                    case 119:
#line 857 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 3114 "parser_gen.cpp"
                    break;

                    case 120:
#line 860 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 3122 "parser_gen.cpp"
                    break;

                    case 121:
#line 863 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 3130 "parser_gen.cpp"
                    break;

                    case 122:
#line 866 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 3138 "parser_gen.cpp"
                    break;

                    case 123:
#line 869 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 3146 "parser_gen.cpp"
                    break;

                    case 124:
#line 872 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 3154 "parser_gen.cpp"
                    break;

                    case 125:
#line 875 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 3162 "parser_gen.cpp"
                    break;

                    case 126:
#line 878 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 3170 "parser_gen.cpp"
                    break;

                    case 127:
#line 881 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 3178 "parser_gen.cpp"
                    break;

                    case 128:
#line 884 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 3186 "parser_gen.cpp"
                    break;

                    case 129:
#line 887 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 3194 "parser_gen.cpp"
                    break;

                    case 130:
#line 890 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3202 "parser_gen.cpp"
                    break;

                    case 131:
#line 893 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3210 "parser_gen.cpp"
                    break;

                    case 132:
#line 896 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3218 "parser_gen.cpp"
                    break;

                    case 133:
#line 899 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3226 "parser_gen.cpp"
                    break;

                    case 134:
#line 902 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"hour"};
                    }
#line 3234 "parser_gen.cpp"
                    break;

                    case 135:
#line 905 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"year"};
                    }
#line 3242 "parser_gen.cpp"
                    break;

                    case 136:
#line 908 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"minute"};
                    }
#line 3250 "parser_gen.cpp"
                    break;

                    case 137:
#line 911 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"second"};
                    }
#line 3258 "parser_gen.cpp"
                    break;

                    case 138:
#line 914 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"millisecond"};
                    }
#line 3266 "parser_gen.cpp"
                    break;

                    case 139:
#line 917 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"day"};
                    }
#line 3274 "parser_gen.cpp"
                    break;

                    case 140:
#line 920 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoDayOfWeek"};
                    }
#line 3282 "parser_gen.cpp"
                    break;

                    case 141:
#line 923 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeek"};
                    }
#line 3290 "parser_gen.cpp"
                    break;

                    case 142:
#line 926 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeekYear"};
                    }
#line 3298 "parser_gen.cpp"
                    break;

                    case 143:
#line 929 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"iso8601"};
                    }
#line 3306 "parser_gen.cpp"
                    break;

                    case 144:
#line 932 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"month"};
                    }
#line 3314 "parser_gen.cpp"
                    break;

                    case 145:
#line 940 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3322 "parser_gen.cpp"
                    break;

                    case 146:
#line 943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3330 "parser_gen.cpp"
                    break;

                    case 147:
#line 946 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3338 "parser_gen.cpp"
                    break;

                    case 148:
#line 949 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3346 "parser_gen.cpp"
                    break;

                    case 149:
#line 952 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3354 "parser_gen.cpp"
                    break;

                    case 150:
#line 955 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3362 "parser_gen.cpp"
                    break;

                    case 151:
#line 958 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3370 "parser_gen.cpp"
                    break;

                    case 152:
#line 961 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3378 "parser_gen.cpp"
                    break;

                    case 153:
#line 964 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3386 "parser_gen.cpp"
                    break;

                    case 154:
#line 967 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3394 "parser_gen.cpp"
                    break;

                    case 155:
#line 970 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3402 "parser_gen.cpp"
                    break;

                    case 156:
#line 973 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3410 "parser_gen.cpp"
                    break;

                    case 157:
#line 976 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3418 "parser_gen.cpp"
                    break;

                    case 158:
#line 979 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3426 "parser_gen.cpp"
                    break;

                    case 159:
#line 982 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3434 "parser_gen.cpp"
                    break;

                    case 160:
#line 985 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3442 "parser_gen.cpp"
                    break;

                    case 161:
#line 988 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3450 "parser_gen.cpp"
                    break;

                    case 162:
#line 991 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3458 "parser_gen.cpp"
                    break;

                    case 163:
#line 994 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3466 "parser_gen.cpp"
                    break;

                    case 164:
#line 997 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3474 "parser_gen.cpp"
                    break;

                    case 165:
#line 1000 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3482 "parser_gen.cpp"
                    break;

                    case 166:
#line 1003 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3490 "parser_gen.cpp"
                    break;

                    case 167:
#line 1006 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3498 "parser_gen.cpp"
                    break;

                    case 168:
#line 1009 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3506 "parser_gen.cpp"
                    break;

                    case 169:
#line 1012 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3514 "parser_gen.cpp"
                    break;

                    case 170:
#line 1015 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3522 "parser_gen.cpp"
                    break;

                    case 171:
#line 1018 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3530 "parser_gen.cpp"
                    break;

                    case 172:
#line 1021 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3538 "parser_gen.cpp"
                    break;

                    case 173:
#line 1024 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3546 "parser_gen.cpp"
                    break;

                    case 174:
#line 1027 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3554 "parser_gen.cpp"
                    break;

                    case 175:
#line 1030 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3562 "parser_gen.cpp"
                    break;

                    case 176:
#line 1033 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3570 "parser_gen.cpp"
                    break;

                    case 177:
#line 1036 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3578 "parser_gen.cpp"
                    break;

                    case 178:
#line 1039 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3586 "parser_gen.cpp"
                    break;

                    case 179:
#line 1042 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3594 "parser_gen.cpp"
                    break;

                    case 180:
#line 1045 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3602 "parser_gen.cpp"
                    break;

                    case 181:
#line 1048 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3610 "parser_gen.cpp"
                    break;

                    case 182:
#line 1051 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3618 "parser_gen.cpp"
                    break;

                    case 183:
#line 1054 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3626 "parser_gen.cpp"
                    break;

                    case 184:
#line 1057 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3634 "parser_gen.cpp"
                    break;

                    case 185:
#line 1060 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3642 "parser_gen.cpp"
                    break;

                    case 186:
#line 1063 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromParts"};
                    }
#line 3650 "parser_gen.cpp"
                    break;

                    case 187:
#line 1066 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToParts"};
                    }
#line 3658 "parser_gen.cpp"
                    break;

                    case 188:
#line 1069 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfMonth"};
                    }
#line 3666 "parser_gen.cpp"
                    break;

                    case 189:
#line 1072 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfWeek"};
                    }
#line 3674 "parser_gen.cpp"
                    break;

                    case 190:
#line 1075 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfYear"};
                    }
#line 3682 "parser_gen.cpp"
                    break;

                    case 191:
#line 1078 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$hour"};
                    }
#line 3690 "parser_gen.cpp"
                    break;

                    case 192:
#line 1081 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoDayOfWeek"};
                    }
#line 3698 "parser_gen.cpp"
                    break;

                    case 193:
#line 1084 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeek"};
                    }
#line 3706 "parser_gen.cpp"
                    break;

                    case 194:
#line 1087 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeekYear"};
                    }
#line 3714 "parser_gen.cpp"
                    break;

                    case 195:
#line 1090 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$millisecond"};
                    }
#line 3722 "parser_gen.cpp"
                    break;

                    case 196:
#line 1093 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$minute"};
                    }
#line 3730 "parser_gen.cpp"
                    break;

                    case 197:
#line 1096 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$month"};
                    }
#line 3738 "parser_gen.cpp"
                    break;

                    case 198:
#line 1099 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$second"};
                    }
#line 3746 "parser_gen.cpp"
                    break;

                    case 199:
#line 1102 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$week"};
                    }
#line 3754 "parser_gen.cpp"
                    break;

                    case 200:
#line 1105 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$year"};
                    }
#line 3762 "parser_gen.cpp"
                    break;

                    case 201:
#line 1108 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3770 "parser_gen.cpp"
                    break;

                    case 202:
#line 1111 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3778 "parser_gen.cpp"
                    break;

                    case 203:
#line 1114 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3786 "parser_gen.cpp"
                    break;

                    case 204:
#line 1117 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3794 "parser_gen.cpp"
                    break;

                    case 205:
#line 1120 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3802 "parser_gen.cpp"
                    break;

                    case 206:
#line 1123 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3810 "parser_gen.cpp"
                    break;

                    case 207:
#line 1126 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3818 "parser_gen.cpp"
                    break;

                    case 208:
#line 1129 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3826 "parser_gen.cpp"
                    break;

                    case 209:
#line 1132 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3834 "parser_gen.cpp"
                    break;

                    case 210:
#line 1135 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3842 "parser_gen.cpp"
                    break;

                    case 211:
#line 1138 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3850 "parser_gen.cpp"
                    break;

                    case 212:
#line 1141 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3858 "parser_gen.cpp"
                    break;

                    case 213:
#line 1144 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3866 "parser_gen.cpp"
                    break;

                    case 214:
#line 1147 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3874 "parser_gen.cpp"
                    break;

                    case 215:
#line 1150 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3882 "parser_gen.cpp"
                    break;

                    case 216:
#line 1153 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3890 "parser_gen.cpp"
                    break;

                    case 217:
#line 1156 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3898 "parser_gen.cpp"
                    break;

                    case 218:
#line 1159 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3906 "parser_gen.cpp"
                    break;

                    case 219:
#line 1162 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3914 "parser_gen.cpp"
                    break;

                    case 220:
#line 1165 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3922 "parser_gen.cpp"
                    break;

                    case 221:
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3930 "parser_gen.cpp"
                    break;

                    case 222:
#line 1171 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3938 "parser_gen.cpp"
                    break;

                    case 223:
#line 1174 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3946 "parser_gen.cpp"
                    break;

                    case 224:
#line 1177 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3954 "parser_gen.cpp"
                    break;

                    case 225:
#line 1180 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3962 "parser_gen.cpp"
                    break;

                    case 226:
#line 1183 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3970 "parser_gen.cpp"
                    break;

                    case 227:
#line 1186 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3978 "parser_gen.cpp"
                    break;

                    case 228:
#line 1189 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3986 "parser_gen.cpp"
                    break;

                    case 229:
#line 1192 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3994 "parser_gen.cpp"
                    break;

                    case 230:
#line 1195 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 4002 "parser_gen.cpp"
                    break;

                    case 231:
#line 1198 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 4010 "parser_gen.cpp"
                    break;

                    case 232:
#line 1201 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 4018 "parser_gen.cpp"
                    break;

                    case 233:
#line 1204 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 4026 "parser_gen.cpp"
                    break;

                    case 234:
#line 1207 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 4034 "parser_gen.cpp"
                    break;

                    case 235:
#line 1210 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 4042 "parser_gen.cpp"
                    break;

                    case 236:
#line 1213 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 4050 "parser_gen.cpp"
                    break;

                    case 237:
#line 1216 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 4058 "parser_gen.cpp"
                    break;

                    case 238:
#line 1219 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 4066 "parser_gen.cpp"
                    break;

                    case 239:
#line 1222 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 4074 "parser_gen.cpp"
                    break;

                    case 240:
#line 1225 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 4082 "parser_gen.cpp"
                    break;

                    case 241:
#line 1228 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 4090 "parser_gen.cpp"
                    break;

                    case 242:
#line 1231 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 4098 "parser_gen.cpp"
                    break;

                    case 243:
#line 1234 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 4106 "parser_gen.cpp"
                    break;

                    case 244:
#line 1241 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 4114 "parser_gen.cpp"
                    break;

                    case 245:
#line 1246 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 4122 "parser_gen.cpp"
                    break;

                    case 246:
#line 1249 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 4130 "parser_gen.cpp"
                    break;

                    case 247:
#line 1252 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 4138 "parser_gen.cpp"
                    break;

                    case 248:
#line 1255 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 4146 "parser_gen.cpp"
                    break;

                    case 249:
#line 1258 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 4154 "parser_gen.cpp"
                    break;

                    case 250:
#line 1261 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 4162 "parser_gen.cpp"
                    break;

                    case 251:
#line 1264 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 4170 "parser_gen.cpp"
                    break;

                    case 252:
#line 1267 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 4178 "parser_gen.cpp"
                    break;

                    case 253:
#line 1270 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 4186 "parser_gen.cpp"
                    break;

                    case 254:
#line 1276 "grammar.yy"
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
#line 4202 "parser_gen.cpp"
                    break;

                    case 255:
#line 1290 "grammar.yy"
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
#line 4218 "parser_gen.cpp"
                    break;

                    case 256:
#line 1304 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 4226 "parser_gen.cpp"
                    break;

                    case 257:
#line 1310 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 4234 "parser_gen.cpp"
                    break;

                    case 258:
#line 1316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 4242 "parser_gen.cpp"
                    break;

                    case 259:
#line 1322 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 4250 "parser_gen.cpp"
                    break;

                    case 260:
#line 1328 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 4258 "parser_gen.cpp"
                    break;

                    case 261:
#line 1334 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 4266 "parser_gen.cpp"
                    break;

                    case 262:
#line 1340 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 4274 "parser_gen.cpp"
                    break;

                    case 263:
#line 1346 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 4282 "parser_gen.cpp"
                    break;

                    case 264:
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 4290 "parser_gen.cpp"
                    break;

                    case 265:
#line 1358 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 4298 "parser_gen.cpp"
                    break;

                    case 266:
#line 1364 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 4306 "parser_gen.cpp"
                    break;

                    case 267:
#line 1370 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 4314 "parser_gen.cpp"
                    break;

                    case 268:
#line 1376 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 4322 "parser_gen.cpp"
                    break;

                    case 269:
#line 1382 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 4330 "parser_gen.cpp"
                    break;

                    case 270:
#line 1385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 4338 "parser_gen.cpp"
                    break;

                    case 271:
#line 1388 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 4346 "parser_gen.cpp"
                    break;

                    case 272:
#line 1391 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 4354 "parser_gen.cpp"
                    break;

                    case 273:
#line 1397 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 4362 "parser_gen.cpp"
                    break;

                    case 274:
#line 1400 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 4370 "parser_gen.cpp"
                    break;

                    case 275:
#line 1403 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 4378 "parser_gen.cpp"
                    break;

                    case 276:
#line 1406 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 4386 "parser_gen.cpp"
                    break;

                    case 277:
#line 1412 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 4394 "parser_gen.cpp"
                    break;

                    case 278:
#line 1415 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 4402 "parser_gen.cpp"
                    break;

                    case 279:
#line 1418 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4410 "parser_gen.cpp"
                    break;

                    case 280:
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4418 "parser_gen.cpp"
                    break;

                    case 281:
#line 1427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4426 "parser_gen.cpp"
                    break;

                    case 282:
#line 1430 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4434 "parser_gen.cpp"
                    break;

                    case 283:
#line 1433 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4442 "parser_gen.cpp"
                    break;

                    case 284:
#line 1436 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4450 "parser_gen.cpp"
                    break;

                    case 285:
#line 1442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4458 "parser_gen.cpp"
                    break;

                    case 286:
#line 1445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4466 "parser_gen.cpp"
                    break;

                    case 287:
#line 1451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4472 "parser_gen.cpp"
                    break;

                    case 288:
#line 1452 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4478 "parser_gen.cpp"
                    break;

                    case 289:
#line 1453 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4484 "parser_gen.cpp"
                    break;

                    case 290:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4490 "parser_gen.cpp"
                    break;

                    case 291:
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4496 "parser_gen.cpp"
                    break;

                    case 292:
#line 1456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4502 "parser_gen.cpp"
                    break;

                    case 293:
#line 1457 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4508 "parser_gen.cpp"
                    break;

                    case 294:
#line 1458 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4514 "parser_gen.cpp"
                    break;

                    case 295:
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4520 "parser_gen.cpp"
                    break;

                    case 296:
#line 1460 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4526 "parser_gen.cpp"
                    break;

                    case 297:
#line 1461 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4532 "parser_gen.cpp"
                    break;

                    case 298:
#line 1462 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4538 "parser_gen.cpp"
                    break;

                    case 299:
#line 1463 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4544 "parser_gen.cpp"
                    break;

                    case 300:
#line 1464 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4550 "parser_gen.cpp"
                    break;

                    case 301:
#line 1465 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4556 "parser_gen.cpp"
                    break;

                    case 302:
#line 1466 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4562 "parser_gen.cpp"
                    break;

                    case 303:
#line 1467 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4568 "parser_gen.cpp"
                    break;

                    case 304:
#line 1468 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4574 "parser_gen.cpp"
                    break;

                    case 305:
#line 1469 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4580 "parser_gen.cpp"
                    break;

                    case 306:
#line 1470 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4586 "parser_gen.cpp"
                    break;

                    case 307:
#line 1471 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4592 "parser_gen.cpp"
                    break;

                    case 308:
#line 1478 "grammar.yy"
                    {
                    }
#line 4598 "parser_gen.cpp"
                    break;

                    case 309:
#line 1479 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4607 "parser_gen.cpp"
                    break;

                    case 310:
#line 1486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4613 "parser_gen.cpp"
                    break;

                    case 311:
#line 1486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4619 "parser_gen.cpp"
                    break;

                    case 312:
#line 1486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4625 "parser_gen.cpp"
                    break;

                    case 313:
#line 1486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4631 "parser_gen.cpp"
                    break;

                    case 314:
#line 1490 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4637 "parser_gen.cpp"
                    break;

                    case 315:
#line 1490 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4643 "parser_gen.cpp"
                    break;

                    case 316:
#line 1494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4649 "parser_gen.cpp"
                    break;

                    case 317:
#line 1494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4655 "parser_gen.cpp"
                    break;

                    case 318:
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4661 "parser_gen.cpp"
                    break;

                    case 319:
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4667 "parser_gen.cpp"
                    break;

                    case 320:
#line 1502 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4673 "parser_gen.cpp"
                    break;

                    case 321:
#line 1502 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4679 "parser_gen.cpp"
                    break;

                    case 322:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4685 "parser_gen.cpp"
                    break;

                    case 323:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4691 "parser_gen.cpp"
                    break;

                    case 324:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4697 "parser_gen.cpp"
                    break;

                    case 325:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4703 "parser_gen.cpp"
                    break;

                    case 326:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4709 "parser_gen.cpp"
                    break;

                    case 327:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4715 "parser_gen.cpp"
                    break;

                    case 328:
#line 1506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4721 "parser_gen.cpp"
                    break;

                    case 329:
#line 1507 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4727 "parser_gen.cpp"
                    break;

                    case 330:
#line 1507 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4733 "parser_gen.cpp"
                    break;

                    case 331:
#line 1507 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4739 "parser_gen.cpp"
                    break;

                    case 332:
#line 1512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4747 "parser_gen.cpp"
                    break;

                    case 333:
#line 1519 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4755 "parser_gen.cpp"
                    break;

                    case 334:
#line 1525 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4764 "parser_gen.cpp"
                    break;

                    case 335:
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4773 "parser_gen.cpp"
                    break;

                    case 336:
#line 1538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4781 "parser_gen.cpp"
                    break;

                    case 337:
#line 1545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4789 "parser_gen.cpp"
                    break;

                    case 338:
#line 1550 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4795 "parser_gen.cpp"
                    break;

                    case 339:
#line 1550 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4801 "parser_gen.cpp"
                    break;

                    case 340:
#line 1555 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4809 "parser_gen.cpp"
                    break;

                    case 341:
#line 1561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4817 "parser_gen.cpp"
                    break;

                    case 342:
#line 1564 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4826 "parser_gen.cpp"
                    break;

                    case 343:
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4834 "parser_gen.cpp"
                    break;

                    case 344:
#line 1578 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4840 "parser_gen.cpp"
                    break;

                    case 345:
#line 1578 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4846 "parser_gen.cpp"
                    break;

                    case 346:
#line 1578 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4852 "parser_gen.cpp"
                    break;

                    case 347:
#line 1582 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4860 "parser_gen.cpp"
                    break;

                    case 348:
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$elemMatch"};
                    }
#line 4868 "parser_gen.cpp"
                    break;

                    case 349:
#line 1594 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 4876 "parser_gen.cpp"
                    break;

                    case 350:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4882 "parser_gen.cpp"
                    break;

                    case 351:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4888 "parser_gen.cpp"
                    break;

                    case 352:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4894 "parser_gen.cpp"
                    break;

                    case 353:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4900 "parser_gen.cpp"
                    break;

                    case 354:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4906 "parser_gen.cpp"
                    break;

                    case 355:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4912 "parser_gen.cpp"
                    break;

                    case 356:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4918 "parser_gen.cpp"
                    break;

                    case 357:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4924 "parser_gen.cpp"
                    break;

                    case 358:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4930 "parser_gen.cpp"
                    break;

                    case 359:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4936 "parser_gen.cpp"
                    break;

                    case 360:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4942 "parser_gen.cpp"
                    break;

                    case 361:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4948 "parser_gen.cpp"
                    break;

                    case 362:
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4954 "parser_gen.cpp"
                    break;

                    case 363:
#line 1601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4960 "parser_gen.cpp"
                    break;

                    case 364:
#line 1601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4966 "parser_gen.cpp"
                    break;

                    case 365:
#line 1601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4972 "parser_gen.cpp"
                    break;

                    case 366:
#line 1605 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4980 "parser_gen.cpp"
                    break;

                    case 367:
#line 1608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4988 "parser_gen.cpp"
                    break;

                    case 368:
#line 1611 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4996 "parser_gen.cpp"
                    break;

                    case 369:
#line 1614 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 5004 "parser_gen.cpp"
                    break;

                    case 370:
#line 1617 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 5012 "parser_gen.cpp"
                    break;

                    case 371:
#line 1620 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 5020 "parser_gen.cpp"
                    break;

                    case 372:
#line 1623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 5028 "parser_gen.cpp"
                    break;

                    case 373:
#line 1626 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 5036 "parser_gen.cpp"
                    break;

                    case 374:
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 5044 "parser_gen.cpp"
                    break;

                    case 375:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5050 "parser_gen.cpp"
                    break;

                    case 376:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5056 "parser_gen.cpp"
                    break;

                    case 377:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5062 "parser_gen.cpp"
                    break;

                    case 378:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5068 "parser_gen.cpp"
                    break;

                    case 379:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5074 "parser_gen.cpp"
                    break;

                    case 380:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5080 "parser_gen.cpp"
                    break;

                    case 381:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5086 "parser_gen.cpp"
                    break;

                    case 382:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5092 "parser_gen.cpp"
                    break;

                    case 383:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5098 "parser_gen.cpp"
                    break;

                    case 384:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5104 "parser_gen.cpp"
                    break;

                    case 385:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5110 "parser_gen.cpp"
                    break;

                    case 386:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5116 "parser_gen.cpp"
                    break;

                    case 387:
#line 1634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5122 "parser_gen.cpp"
                    break;

                    case 388:
#line 1635 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5128 "parser_gen.cpp"
                    break;

                    case 389:
#line 1635 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5134 "parser_gen.cpp"
                    break;

                    case 390:
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5143 "parser_gen.cpp"
                    break;

                    case 391:
#line 1646 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5152 "parser_gen.cpp"
                    break;

                    case 392:
#line 1652 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5160 "parser_gen.cpp"
                    break;

                    case 393:
#line 1657 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5168 "parser_gen.cpp"
                    break;

                    case 394:
#line 1662 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5177 "parser_gen.cpp"
                    break;

                    case 395:
#line 1668 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5185 "parser_gen.cpp"
                    break;

                    case 396:
#line 1673 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5193 "parser_gen.cpp"
                    break;

                    case 397:
#line 1678 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5201 "parser_gen.cpp"
                    break;

                    case 398:
#line 1683 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5210 "parser_gen.cpp"
                    break;

                    case 399:
#line 1689 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5218 "parser_gen.cpp"
                    break;

                    case 400:
#line 1694 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5227 "parser_gen.cpp"
                    break;

                    case 401:
#line 1700 "grammar.yy"
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
#line 5239 "parser_gen.cpp"
                    break;

                    case 402:
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5248 "parser_gen.cpp"
                    break;

                    case 403:
#line 1715 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5257 "parser_gen.cpp"
                    break;

                    case 404:
#line 1721 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5265 "parser_gen.cpp"
                    break;

                    case 405:
#line 1726 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5274 "parser_gen.cpp"
                    break;

                    case 406:
#line 1732 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5283 "parser_gen.cpp"
                    break;

                    case 407:
#line 1738 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5291 "parser_gen.cpp"
                    break;

                    case 408:
#line 1743 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5299 "parser_gen.cpp"
                    break;

                    case 409:
#line 1748 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5307 "parser_gen.cpp"
                    break;

                    case 410:
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5315 "parser_gen.cpp"
                    break;

                    case 411:
#line 1758 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5323 "parser_gen.cpp"
                    break;

                    case 412:
#line 1763 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5331 "parser_gen.cpp"
                    break;

                    case 413:
#line 1768 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5339 "parser_gen.cpp"
                    break;

                    case 414:
#line 1773 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5347 "parser_gen.cpp"
                    break;

                    case 415:
#line 1778 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5355 "parser_gen.cpp"
                    break;

                    case 416:
#line 1783 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5363 "parser_gen.cpp"
                    break;

                    case 417:
#line 1788 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5371 "parser_gen.cpp"
                    break;

                    case 418:
#line 1793 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5379 "parser_gen.cpp"
                    break;

                    case 419:
#line 1798 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5387 "parser_gen.cpp"
                    break;

                    case 420:
#line 1803 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5395 "parser_gen.cpp"
                    break;

                    case 421:
#line 1809 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5401 "parser_gen.cpp"
                    break;

                    case 422:
#line 1809 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5407 "parser_gen.cpp"
                    break;

                    case 423:
#line 1809 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5413 "parser_gen.cpp"
                    break;

                    case 424:
#line 1813 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5422 "parser_gen.cpp"
                    break;

                    case 425:
#line 1820 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5431 "parser_gen.cpp"
                    break;

                    case 426:
#line 1827 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5440 "parser_gen.cpp"
                    break;

                    case 427:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5446 "parser_gen.cpp"
                    break;

                    case 428:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5452 "parser_gen.cpp"
                    break;

                    case 429:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5458 "parser_gen.cpp"
                    break;

                    case 430:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5464 "parser_gen.cpp"
                    break;

                    case 431:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5470 "parser_gen.cpp"
                    break;

                    case 432:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5476 "parser_gen.cpp"
                    break;

                    case 433:
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5482 "parser_gen.cpp"
                    break;

                    case 434:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5488 "parser_gen.cpp"
                    break;

                    case 435:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5494 "parser_gen.cpp"
                    break;

                    case 436:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5500 "parser_gen.cpp"
                    break;

                    case 437:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5506 "parser_gen.cpp"
                    break;

                    case 438:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5512 "parser_gen.cpp"
                    break;

                    case 439:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5518 "parser_gen.cpp"
                    break;

                    case 440:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5524 "parser_gen.cpp"
                    break;

                    case 441:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5530 "parser_gen.cpp"
                    break;

                    case 442:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5536 "parser_gen.cpp"
                    break;

                    case 443:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5542 "parser_gen.cpp"
                    break;

                    case 444:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5548 "parser_gen.cpp"
                    break;

                    case 445:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5554 "parser_gen.cpp"
                    break;

                    case 446:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5560 "parser_gen.cpp"
                    break;

                    case 447:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5566 "parser_gen.cpp"
                    break;

                    case 448:
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5572 "parser_gen.cpp"
                    break;

                    case 449:
#line 1840 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5584 "parser_gen.cpp"
                    break;

                    case 450:
#line 1850 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5592 "parser_gen.cpp"
                    break;

                    case 451:
#line 1853 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5600 "parser_gen.cpp"
                    break;

                    case 452:
#line 1859 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5608 "parser_gen.cpp"
                    break;

                    case 453:
#line 1862 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5616 "parser_gen.cpp"
                    break;

                    case 454:
#line 1870 "grammar.yy"
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
#line 5626 "parser_gen.cpp"
                    break;

                    case 455:
#line 1879 "grammar.yy"
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
#line 5636 "parser_gen.cpp"
                    break;

                    case 456:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5642 "parser_gen.cpp"
                    break;

                    case 457:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5648 "parser_gen.cpp"
                    break;

                    case 458:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5654 "parser_gen.cpp"
                    break;

                    case 459:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5660 "parser_gen.cpp"
                    break;

                    case 460:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5666 "parser_gen.cpp"
                    break;

                    case 461:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5672 "parser_gen.cpp"
                    break;

                    case 462:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5678 "parser_gen.cpp"
                    break;

                    case 463:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5684 "parser_gen.cpp"
                    break;

                    case 464:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5690 "parser_gen.cpp"
                    break;

                    case 465:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5696 "parser_gen.cpp"
                    break;

                    case 466:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5702 "parser_gen.cpp"
                    break;

                    case 467:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5708 "parser_gen.cpp"
                    break;

                    case 468:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5714 "parser_gen.cpp"
                    break;

                    case 469:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5720 "parser_gen.cpp"
                    break;

                    case 470:
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5726 "parser_gen.cpp"
                    break;

                    case 471:
#line 1892 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
                    }
#line 5734 "parser_gen.cpp"
                    break;

                    case 472:
#line 1895 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::hourArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5742 "parser_gen.cpp"
                    break;

                    case 473:
#line 1901 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
                    }
#line 5750 "parser_gen.cpp"
                    break;

                    case 474:
#line 1904 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::minuteArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5758 "parser_gen.cpp"
                    break;

                    case 475:
#line 1910 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
                    }
#line 5766 "parser_gen.cpp"
                    break;

                    case 476:
#line 1913 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::secondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5774 "parser_gen.cpp"
                    break;

                    case 477:
#line 1919 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
                    }
#line 5782 "parser_gen.cpp"
                    break;

                    case 478:
#line 1922 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::millisecondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5790 "parser_gen.cpp"
                    break;

                    case 479:
#line 1928 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
                    }
#line 5798 "parser_gen.cpp"
                    break;

                    case 480:
#line 1931 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5806 "parser_gen.cpp"
                    break;

                    case 481:
#line 1937 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5814 "parser_gen.cpp"
                    break;

                    case 482:
#line 1940 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoDayOfWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5822 "parser_gen.cpp"
                    break;

                    case 483:
#line 1946 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5830 "parser_gen.cpp"
                    break;

                    case 484:
#line 1949 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5838 "parser_gen.cpp"
                    break;

                    case 485:
#line 1955 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
                    }
#line 5846 "parser_gen.cpp"
                    break;

                    case 486:
#line 1958 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::iso8601Arg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5854 "parser_gen.cpp"
                    break;

                    case 487:
#line 1964 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
                    }
#line 5862 "parser_gen.cpp"
                    break;

                    case 488:
#line 1967 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::monthArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5870 "parser_gen.cpp"
                    break;

                    case 489:
#line 1974 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateFromParts,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::yearArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[6].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[10].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[9].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[7].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[8].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5880 "parser_gen.cpp"
                    break;

                    case 490:
#line 1980 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateFromParts,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::isoWeekYearArg,
                                  YY_MOVE(yystack_[7].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[9].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[10].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[11].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[5].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(
                                     yystack_[6].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5890 "parser_gen.cpp"
                    break;

                    case 491:
#line 1988 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dateToParts,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 YY_MOVE(
                                     yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 YY_MOVE(yystack_[3]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5900 "parser_gen.cpp"
                    break;

                    case 492:
#line 1996 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5908 "parser_gen.cpp"
                    break;

                    case 493:
#line 1999 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5917 "parser_gen.cpp"
                    break;

                    case 494:
#line 2003 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5925 "parser_gen.cpp"
                    break;

                    case 495:
#line 2009 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5934 "parser_gen.cpp"
                    break;

                    case 496:
#line 2013 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5943 "parser_gen.cpp"
                    break;

                    case 497:
#line 2017 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5951 "parser_gen.cpp"
                    break;

                    case 498:
#line 2023 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5960 "parser_gen.cpp"
                    break;

                    case 499:
#line 2027 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5969 "parser_gen.cpp"
                    break;

                    case 500:
#line 2031 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5977 "parser_gen.cpp"
                    break;

                    case 501:
#line 2037 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5986 "parser_gen.cpp"
                    break;

                    case 502:
#line 2041 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5995 "parser_gen.cpp"
                    break;

                    case 503:
#line 2045 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6003 "parser_gen.cpp"
                    break;

                    case 504:
#line 2051 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6012 "parser_gen.cpp"
                    break;

                    case 505:
#line 2055 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6021 "parser_gen.cpp"
                    break;

                    case 506:
#line 2059 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6029 "parser_gen.cpp"
                    break;

                    case 507:
#line 2065 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6038 "parser_gen.cpp"
                    break;

                    case 508:
#line 2069 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6047 "parser_gen.cpp"
                    break;

                    case 509:
#line 2073 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6055 "parser_gen.cpp"
                    break;

                    case 510:
#line 2079 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6064 "parser_gen.cpp"
                    break;

                    case 511:
#line 2083 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6073 "parser_gen.cpp"
                    break;

                    case 512:
#line 2087 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6081 "parser_gen.cpp"
                    break;

                    case 513:
#line 2093 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6090 "parser_gen.cpp"
                    break;

                    case 514:
#line 2097 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6099 "parser_gen.cpp"
                    break;

                    case 515:
#line 2101 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6107 "parser_gen.cpp"
                    break;

                    case 516:
#line 2107 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6116 "parser_gen.cpp"
                    break;

                    case 517:
#line 2111 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6125 "parser_gen.cpp"
                    break;

                    case 518:
#line 2115 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6133 "parser_gen.cpp"
                    break;

                    case 519:
#line 2121 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6142 "parser_gen.cpp"
                    break;

                    case 520:
#line 2125 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6151 "parser_gen.cpp"
                    break;

                    case 521:
#line 2129 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6159 "parser_gen.cpp"
                    break;

                    case 522:
#line 2135 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6168 "parser_gen.cpp"
                    break;

                    case 523:
#line 2139 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6177 "parser_gen.cpp"
                    break;

                    case 524:
#line 2143 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6185 "parser_gen.cpp"
                    break;

                    case 525:
#line 2149 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6194 "parser_gen.cpp"
                    break;

                    case 526:
#line 2153 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6203 "parser_gen.cpp"
                    break;

                    case 527:
#line 2157 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6211 "parser_gen.cpp"
                    break;

                    case 528:
#line 2163 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6220 "parser_gen.cpp"
                    break;

                    case 529:
#line 2167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6229 "parser_gen.cpp"
                    break;

                    case 530:
#line 2171 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6237 "parser_gen.cpp"
                    break;

                    case 531:
#line 2177 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 6245 "parser_gen.cpp"
                    break;

                    case 532:
#line 2180 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6253 "parser_gen.cpp"
                    break;

                    case 533:
#line 2183 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6261 "parser_gen.cpp"
                    break;

                    case 534:
#line 2190 "grammar.yy"
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
#line 6273 "parser_gen.cpp"
                    break;

                    case 535:
#line 2201 "grammar.yy"
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
#line 6285 "parser_gen.cpp"
                    break;

                    case 536:
#line 2211 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 6293 "parser_gen.cpp"
                    break;

                    case 537:
#line 2214 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6301 "parser_gen.cpp"
                    break;

                    case 538:
#line 2220 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6311 "parser_gen.cpp"
                    break;

                    case 539:
#line 2228 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6321 "parser_gen.cpp"
                    break;

                    case 540:
#line 2236 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6331 "parser_gen.cpp"
                    break;

                    case 541:
#line 2244 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 6339 "parser_gen.cpp"
                    break;

                    case 542:
#line 2247 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6347 "parser_gen.cpp"
                    break;

                    case 543:
#line 2252 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 6359 "parser_gen.cpp"
                    break;

                    case 544:
#line 2261 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6367 "parser_gen.cpp"
                    break;

                    case 545:
#line 2267 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6375 "parser_gen.cpp"
                    break;

                    case 546:
#line 2273 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6383 "parser_gen.cpp"
                    break;

                    case 547:
#line 2280 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6394 "parser_gen.cpp"
                    break;

                    case 548:
#line 2290 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6405 "parser_gen.cpp"
                    break;

                    case 549:
#line 2299 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6414 "parser_gen.cpp"
                    break;

                    case 550:
#line 2306 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6423 "parser_gen.cpp"
                    break;

                    case 551:
#line 2313 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6432 "parser_gen.cpp"
                    break;

                    case 552:
#line 2321 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6441 "parser_gen.cpp"
                    break;

                    case 553:
#line 2329 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6450 "parser_gen.cpp"
                    break;

                    case 554:
#line 2337 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6459 "parser_gen.cpp"
                    break;

                    case 555:
#line 2345 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6468 "parser_gen.cpp"
                    break;

                    case 556:
#line 2352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6476 "parser_gen.cpp"
                    break;

                    case 557:
#line 2358 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6484 "parser_gen.cpp"
                    break;

                    case 558:
#line 2364 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 6492 "parser_gen.cpp"
                    break;

                    case 559:
#line 2367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 6500 "parser_gen.cpp"
                    break;

                    case 560:
#line 2373 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6508 "parser_gen.cpp"
                    break;

                    case 561:
#line 2379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6516 "parser_gen.cpp"
                    break;

                    case 562:
#line 2384 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6524 "parser_gen.cpp"
                    break;

                    case 563:
#line 2387 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6533 "parser_gen.cpp"
                    break;

                    case 564:
#line 2394 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 6541 "parser_gen.cpp"
                    break;

                    case 565:
#line 2397 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 6549 "parser_gen.cpp"
                    break;

                    case 566:
#line 2400 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 6557 "parser_gen.cpp"
                    break;

                    case 567:
#line 2403 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 6565 "parser_gen.cpp"
                    break;

                    case 568:
#line 2406 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 6573 "parser_gen.cpp"
                    break;

                    case 569:
#line 2409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 6581 "parser_gen.cpp"
                    break;

                    case 570:
#line 2412 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 6589 "parser_gen.cpp"
                    break;

                    case 571:
#line 2415 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 6597 "parser_gen.cpp"
                    break;

                    case 572:
#line 2420 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 6605 "parser_gen.cpp"
                    break;

                    case 573:
#line 2422 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 6617 "parser_gen.cpp"
                    break;

                    case 574:
#line 2432 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6625 "parser_gen.cpp"
                    break;

                    case 575:
#line 2434 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6633 "parser_gen.cpp"
                    break;

                    case 576:
#line 2440 "grammar.yy"
                    {
                        auto&& fields = YY_MOVE(yystack_[1].value.as<CNode>());
                        if (auto status =
                                c_node_validation::validateNoConflictingPathsInProjectFields(
                                    fields);
                            !status.isOK())
                            error(yystack_[2].location, status.reason());
                        if (auto inclusion =
                                c_node_validation::validateProjectionAsInclusionOrExclusion(fields);
                            inclusion.isOK())
                            yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                                inclusion.getValue() == c_node_validation::IsInclusion::yes
                                    ? KeyFieldname::projectInclusion
                                    : KeyFieldname::projectExclusion,
                                std::move(fields)}}};
                        else
                            // Pass the location of the project token to the error reporting
                            // function.
                            error(yystack_[2].location, inclusion.getStatus().reason());
                    }
#line 6654 "parser_gen.cpp"
                    break;

                    case 577:
#line 2459 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6662 "parser_gen.cpp"
                    break;

                    case 578:
#line 2462 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6671 "parser_gen.cpp"
                    break;

                    case 579:
#line 2469 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6679 "parser_gen.cpp"
                    break;

                    case 580:
#line 2472 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6687 "parser_gen.cpp"
                    break;

                    case 581:
#line 2478 "grammar.yy"
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
#line 6703 "parser_gen.cpp"
                    break;

                    case 582:
#line 2492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6709 "parser_gen.cpp"
                    break;

                    case 583:
#line 2493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6715 "parser_gen.cpp"
                    break;

                    case 584:
#line 2494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6721 "parser_gen.cpp"
                    break;

                    case 585:
#line 2495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6727 "parser_gen.cpp"
                    break;

                    case 586:
#line 2496 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6733 "parser_gen.cpp"
                    break;

                    case 587:
#line 2500 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = {CNode::ObjectChildren{
                            {KeyFieldname::elemMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6741 "parser_gen.cpp"
                    break;

                    case 588:
#line 2506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6749 "parser_gen.cpp"
                    break;

                    case 589:
#line 2509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6758 "parser_gen.cpp"
                    break;

                    case 590:
#line 2517 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6766 "parser_gen.cpp"
                    break;

                    case 591:
#line 2524 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6775 "parser_gen.cpp"
                    break;

                    case 592:
#line 2528 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6784 "parser_gen.cpp"
                    break;

                    case 593:
#line 2536 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6792 "parser_gen.cpp"
                    break;

                    case 594:
#line 2539 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6800 "parser_gen.cpp"
                    break;

                    case 595:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6806 "parser_gen.cpp"
                    break;

                    case 596:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6812 "parser_gen.cpp"
                    break;

                    case 597:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6818 "parser_gen.cpp"
                    break;

                    case 598:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6824 "parser_gen.cpp"
                    break;

                    case 599:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6830 "parser_gen.cpp"
                    break;

                    case 600:
#line 2545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6836 "parser_gen.cpp"
                    break;

                    case 601:
#line 2546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6842 "parser_gen.cpp"
                    break;

                    case 602:
#line 2550 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6850 "parser_gen.cpp"
                    break;

                    case 603:
#line 2556 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6858 "parser_gen.cpp"
                    break;

                    case 604:
#line 2562 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6867 "parser_gen.cpp"
                    break;

                    case 605:
#line 2570 "grammar.yy"
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
#line 6879 "parser_gen.cpp"
                    break;

                    case 606:
#line 2581 "grammar.yy"
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
#line 6891 "parser_gen.cpp"
                    break;

                    case 607:
#line 2591 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6900 "parser_gen.cpp"
                    break;

                    case 608:
#line 2599 "grammar.yy"
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
#line 6912 "parser_gen.cpp"
                    break;

                    case 609:
#line 2609 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6918 "parser_gen.cpp"
                    break;

                    case 610:
#line 2609 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6924 "parser_gen.cpp"
                    break;

                    case 611:
#line 2613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6933 "parser_gen.cpp"
                    break;

                    case 612:
#line 2620 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6942 "parser_gen.cpp"
                    break;

                    case 613:
#line 2627 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6948 "parser_gen.cpp"
                    break;

                    case 614:
#line 2627 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6954 "parser_gen.cpp"
                    break;

                    case 615:
#line 2631 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6960 "parser_gen.cpp"
                    break;

                    case 616:
#line 2631 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6966 "parser_gen.cpp"
                    break;

                    case 617:
#line 2635 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 6974 "parser_gen.cpp"
                    break;

                    case 618:
#line 2641 "grammar.yy"
                    {
                    }
#line 6980 "parser_gen.cpp"
                    break;

                    case 619:
#line 2642 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 6989 "parser_gen.cpp"
                    break;

                    case 620:
#line 2649 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6997 "parser_gen.cpp"
                    break;

                    case 621:
#line 2655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7005 "parser_gen.cpp"
                    break;

                    case 622:
#line 2658 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7014 "parser_gen.cpp"
                    break;

                    case 623:
#line 2665 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7022 "parser_gen.cpp"
                    break;

                    case 624:
#line 2672 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7028 "parser_gen.cpp"
                    break;

                    case 625:
#line 2673 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7034 "parser_gen.cpp"
                    break;

                    case 626:
#line 2674 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7040 "parser_gen.cpp"
                    break;

                    case 627:
#line 2675 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7046 "parser_gen.cpp"
                    break;

                    case 628:
#line 2676 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7052 "parser_gen.cpp"
                    break;

                    case 629:
#line 2677 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7058 "parser_gen.cpp"
                    break;

                    case 630:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7064 "parser_gen.cpp"
                    break;

                    case 631:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7070 "parser_gen.cpp"
                    break;

                    case 632:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7076 "parser_gen.cpp"
                    break;

                    case 633:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7082 "parser_gen.cpp"
                    break;

                    case 634:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7088 "parser_gen.cpp"
                    break;

                    case 635:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7094 "parser_gen.cpp"
                    break;

                    case 636:
#line 2680 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7100 "parser_gen.cpp"
                    break;

                    case 637:
#line 2682 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7109 "parser_gen.cpp"
                    break;

                    case 638:
#line 2687 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7118 "parser_gen.cpp"
                    break;

                    case 639:
#line 2692 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7127 "parser_gen.cpp"
                    break;

                    case 640:
#line 2697 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7136 "parser_gen.cpp"
                    break;

                    case 641:
#line 2702 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7145 "parser_gen.cpp"
                    break;

                    case 642:
#line 2707 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7154 "parser_gen.cpp"
                    break;

                    case 643:
#line 2712 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7163 "parser_gen.cpp"
                    break;

                    case 644:
#line 2718 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7169 "parser_gen.cpp"
                    break;

                    case 645:
#line 2719 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7175 "parser_gen.cpp"
                    break;

                    case 646:
#line 2720 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7181 "parser_gen.cpp"
                    break;

                    case 647:
#line 2721 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7187 "parser_gen.cpp"
                    break;

                    case 648:
#line 2722 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7193 "parser_gen.cpp"
                    break;

                    case 649:
#line 2723 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7199 "parser_gen.cpp"
                    break;

                    case 650:
#line 2724 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7205 "parser_gen.cpp"
                    break;

                    case 651:
#line 2725 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7211 "parser_gen.cpp"
                    break;

                    case 652:
#line 2726 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7217 "parser_gen.cpp"
                    break;

                    case 653:
#line 2727 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7223 "parser_gen.cpp"
                    break;

                    case 654:
#line 2732 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 7231 "parser_gen.cpp"
                    break;

                    case 655:
#line 2735 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7239 "parser_gen.cpp"
                    break;

                    case 656:
#line 2742 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 7247 "parser_gen.cpp"
                    break;

                    case 657:
#line 2745 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7255 "parser_gen.cpp"
                    break;

                    case 658:
#line 2752 "grammar.yy"
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
#line 7266 "parser_gen.cpp"
                    break;

                    case 659:
#line 2761 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7274 "parser_gen.cpp"
                    break;

                    case 660:
#line 2766 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7282 "parser_gen.cpp"
                    break;

                    case 661:
#line 2771 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7290 "parser_gen.cpp"
                    break;

                    case 662:
#line 2776 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7298 "parser_gen.cpp"
                    break;

                    case 663:
#line 2781 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7306 "parser_gen.cpp"
                    break;

                    case 664:
#line 2786 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7314 "parser_gen.cpp"
                    break;

                    case 665:
#line 2791 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7322 "parser_gen.cpp"
                    break;

                    case 666:
#line 2796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7330 "parser_gen.cpp"
                    break;

                    case 667:
#line 2801 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7338 "parser_gen.cpp"
                    break;


#line 7342 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -1040;

const short ParserGen::yytable_ninf_ = -482;

const short ParserGen::yypact_[] = {
    -86,   -103,  -100,  -93,   -88,   56,    -83,   -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, 35,    -7,    222,   253,   1598,  -75,   280,   -64,   -61,   280,   -59,
    16,    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, 3574,  -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, 4166,  -1040, -1040, -1040, -1040, -76,   -1040,
    4314,  -1040, -1040, 4314,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, 319,   -1040, -1040, -1040, -1040, 21,    -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, 59,    -1040, -1040, 92,    -83,   -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, 1765,  -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, 9,     -1040, -1040, -1040, 772,   280,   275,   -1040,
    2390,  2099,  4,     -100,  2538,  3722,  3870,  3870,  -17,   7,     -17,   12,    3870,
    3870,  3870,  13,    3870,  3722,  13,    15,    17,    -59,   3870,  3870,  -59,   -59,
    -59,   -59,   4018,  4018,  4018,  3870,  27,    -100,  13,    3722,  3722,  13,    13,
    4018,  -1040, 41,    42,    4018,  4018,  4018,  53,    3722,  55,    3722,  13,    13,
    -59,   901,   4018,  4018,  58,    4018,  61,    13,    65,    -17,   66,    3870,  -59,
    -59,   -59,   -59,   -59,   67,    -59,   4018,  13,    68,    69,    13,    70,    119,
    3870,  3870,  71,    3722,  75,    3722,  3722,  76,    77,    78,    79,    3870,  3870,
    3722,  3722,  3722,  3722,  3722,  3722,  3722,  3722,  3722,  3722,  -59,   80,    3722,
    4018,  4018,  4314,  4314,  939,   -1040, -77,   -1040, 4462,  4462,  -1040, -1040, 46,
    90,    -1040, -1040, -1040, 3574,  -1040, -1040, 3574,  -110,  950,   -1040, -1040, -1040,
    -1040, 73,    -1040, 2265,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, 57,    3722,
    -1040, -1040, -1040, -1040, -1040, -1040, 81,    88,    153,   3722,  154,   3722,  156,
    157,   158,   3722,  159,   160,   162,   190,   -1040, 3574,  115,   192,   221,   123,
    131,   134,   137,   2265,  -1040, -1040, 223,   225,   164,   226,   227,   285,   229,
    230,   288,   232,   3722,  238,   239,   240,   241,   242,   243,   244,   245,   303,
    3722,  3722,  247,   248,   306,   250,   251,   312,   256,   257,   315,   3574,  259,
    3722,  261,   263,   266,   326,   268,   269,   270,   274,   279,   283,   284,   286,
    287,   291,   292,   332,   294,   300,   337,   3722,  301,   308,   342,   3722,  309,
    3722,  313,   3722,  314,   320,   338,   322,   323,   365,   377,   3722,  326,   325,
    330,   383,   341,   3722,  3722,  343,   3722,  280,   346,   347,   350,   3722,  351,
    3722,  354,   355,   3722,  3722,  3722,  3722,  356,   359,   360,   361,   366,   367,
    370,   371,   372,   373,   374,   375,   326,   3722,  376,   378,   379,   394,   380,
    381,   422,   -1040, -1040, -1040, -1040, -1040, -1040, 384,   1932,  -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, 5,     -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, 311,   1496,  -1040, 386,   -1040, -1040, -1040, 387,   -1040, 389,   -1040,
    -1040, -1040, 3722,  -1040, -1040, -1040, -1040, 2686,  391,   3722,  -1040, -1040, 3722,
    444,   3722,  3722,  3722,  -1040, -1040, 3722,  -1040, -1040, 3722,  -1040, -1040, 3722,
    -1040, 3722,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, 3722,  3722,  3722,
    -1040, -1040, 3722,  -1040, -1040, 3722,  -1040, -1040, 3722,  392,   -1040, 3722,  -1040,
    -1040, -1040, 3722,  449,   -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, 3722,  -1040, -1040, 3722,  3722,  -1040, -1040, 3722,  3722,  -1040, 395,
    -1040, 3722,  -1040, -1040, 3722,  -1040, -1040, 3722,  3722,  3722,  450,   -1040, -1040,
    3722,  -1040, 3722,  3722,  -1040, 3722,  280,   -1040, -1040, -1040, 3722,  -1040, 3722,
    -1040, -1040, 3722,  3722,  3722,  3722,  -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, 451,   3722,  -1040, -1040, -1040, 3722,  -1040, -1040,
    3722,  -1040, 4462,  4462,  -1040, 1170,  402,   -44,   1309,  3722,  403,   406,   -1040,
    3722,  -1040, -1040, -1040, -1040, -1040, 407,   408,   410,   411,   412,   452,   -1040,
    3722,  141,   465,   463,   465,   453,   453,   453,   418,   453,   3722,  3722,  453,
    453,   453,   420,   419,   -1040, 3722,  453,   453,   423,   453,   -1040, 421,   424,
    464,   487,   502,   454,   3722,  453,   -1040, -1040, -1040, 459,   479,   480,   3722,
    3722,  3722,  483,   3722,  484,   453,   453,   -1040, -1040, -1040, -1040, -1040, 485,
    -1040, -1040, 3722,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, 3722,  525,
    -1040, 3722,  3722,  530,   534,   3722,  453,   54,    453,   453,   3722,  490,   496,
    514,   515,   520,   3722,  528,   531,   529,   533,   539,   -1040, 544,   545,   546,
    549,   552,   553,   2834,  -1040, 554,   3722,  569,   3722,  3722,  558,   559,   560,
    2982,  3130,  3278,  561,   562,   563,   566,   571,   572,   568,   573,   574,   576,
    577,   -1040, 3426,  -1040, 3722,  605,   -1040, -1040, 3722,  621,   3722,  626,   -1040,
    452,   -1040, 581,   525,   -1040, 582,   587,   589,   -1040, 592,   -1040, 593,   596,
    599,   600,   604,   -1040, 607,   608,   618,   -1040, 622,   623,   -1040, -1040, 3722,
    642,   664,   -1040, 631,   632,   634,   638,   641,   -1040, -1040, -1040, 643,   644,
    645,   -1040, 646,   -1040, 647,   648,   650,   -1040, 3722,  -1040, 3722,  690,   -1040,
    3722,  525,   657,   658,   -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, 659,   3722,  3722,  -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, 660,   -1040, 3722,  453,   708,   665,
    -1040, 666,   -1040, 667,   671,   672,   -1040, 693,   530,   673,   -1040, 674,   675,
    -1040, 3722,  621,   -1040, -1040, -1040, 677,   690,   678,   453,   -1040, 680,   681,
    -1040};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   7,   2,   77,  3,   577, 4,   562, 5,   1,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   10,  11,  12,  13,  14,  15,  6,   101, 129, 118, 128,
    125, 139, 132, 126, 134, 121, 143, 140, 141, 142, 138, 136, 144, 123, 124, 131, 119, 130, 133,
    137, 120, 127, 122, 135, 0,   76,  347, 103, 102, 109, 107, 108, 106, 0,   116, 78,  80,  81,
    0,   576, 0,   68,  70,  0,   69,  117, 578, 169, 237, 240, 145, 223, 147, 224, 236, 239, 238,
    146, 241, 170, 152, 185, 148, 159, 231, 234, 186, 201, 187, 202, 188, 189, 190, 242, 171, 561,
    348, 153, 172, 173, 154, 155, 191, 203, 204, 192, 193, 194, 149, 174, 175, 176, 156, 157, 205,
    206, 195, 196, 177, 197, 178, 158, 151, 150, 179, 243, 207, 208, 209, 211, 210, 180, 212, 198,
    225, 226, 227, 228, 229, 181, 230, 233, 213, 182, 110, 113, 114, 115, 112, 111, 216, 214, 215,
    217, 218, 219, 183, 232, 235, 160, 161, 162, 163, 164, 165, 220, 166, 167, 222, 221, 184, 168,
    199, 200, 573, 625, 626, 627, 624, 0,   628, 629, 572, 563, 0,   284, 283, 282, 280, 279, 278,
    272, 271, 270, 276, 275, 274, 269, 273, 277, 281, 19,  20,  21,  22,  24,  26,  0,   23,  9,
    0,   7,   286, 285, 245, 246, 247, 248, 249, 250, 251, 252, 618, 621, 253, 244, 254, 255, 256,
    257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 296, 297, 298, 299, 300, 305, 301,
    302, 303, 306, 307, 97,  287, 288, 290, 291, 292, 304, 293, 294, 295, 613, 614, 615, 616, 289,
    84,  82,  79,  104, 62,  61,  58,  57,  60,  54,  53,  56,  46,  45,  48,  50,  49,  52,  308,
    0,   47,  51,  55,  59,  41,  42,  43,  44,  63,  64,  65,  34,  35,  36,  37,  38,  39,  40,
    582, 66,  322, 330, 350, 323, 421, 422, 423, 324, 609, 610, 327, 427, 428, 429, 430, 431, 432,
    433, 434, 435, 436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 446, 448, 447, 325, 630, 631,
    632, 633, 634, 635, 636, 331, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467, 468,
    469, 470, 326, 644, 645, 646, 647, 648, 649, 650, 651, 652, 653, 351, 352, 353, 354, 355, 356,
    357, 358, 359, 360, 361, 362, 363, 364, 365, 328, 595, 596, 597, 598, 599, 600, 601, 329, 375,
    376, 377, 378, 379, 380, 381, 382, 383, 385, 386, 387, 384, 388, 389, 584, 579, 581, 585, 586,
    583, 580, 571, 570, 569, 568, 565, 564, 567, 566, 0,   574, 575, 17,  0,   0,   0,   8,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   349, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   591, 0,   25,  0,   0,   67,
    27,  0,   0,   617, 619, 620, 0,   622, 83,  0,   0,   0,   85,  86,  87,  88,  105, 336, 341,
    310, 309, 321, 312, 311, 313, 320, 0,   0,   314, 318, 338, 315, 319, 339, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   308, 0,   0,   0,   0,   479, 0,   0,   0,
    9,   316, 317, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   536, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   536, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   536, 0,   0,   0,   0,   0,   0,
    0,   0,   594, 593, 590, 592, 558, 559, 0,   0,   28,  30,  31,  32,  33,  29,  16,  0,   623,
    89,  84,  98,  91,  94,  96,  95,  93,  100, 0,   0,   392, 0,   414, 417, 390, 0,   424, 0,
    413, 416, 415, 0,   391, 418, 393, 637, 0,   0,   0,   408, 411, 0,   471, 0,   0,   0,   494,
    492, 0,   497, 495, 0,   503, 501, 0,   419, 0,   587, 638, 395, 396, 639, 640, 506, 504, 0,
    0,   0,   500, 498, 0,   515, 513, 0,   518, 516, 0,   0,   397, 0,   399, 641, 642, 0,   0,
    366, 367, 368, 369, 370, 371, 372, 373, 374, 527, 525, 0,   530, 528, 0,   0,   509, 507, 0,
    0,   643, 0,   425, 0,   420, 544, 0,   545, 546, 0,   0,   0,   0,   524, 522, 0,   604, 0,
    0,   607, 0,   0,   588, 407, 410, 0,   404, 0,   550, 551, 0,   0,   0,   0,   409, 412, 659,
    660, 661, 662, 663, 664, 556, 665, 666, 557, 0,   0,   667, 512, 510, 0,   521, 519, 0,   560,
    0,   0,   72,  0,   0,   0,   0,   0,   0,   0,   340, 0,   345, 344, 346, 342, 337, 0,   0,
    0,   0,   0,   654, 480, 0,   477, 450, 485, 450, 452, 452, 452, 0,   452, 531, 531, 452, 452,
    452, 0,   0,   537, 0,   452, 452, 0,   452, 308, 0,   0,   541, 0,   0,   0,   0,   452, 308,
    308, 308, 0,   0,   0,   0,   0,   0,   0,   0,   0,   452, 452, 75,  74,  71,  73,  18,  85,
    90,  92,  0,   334, 335, 343, 602, 603, 332, 449, 611, 0,   656, 472, 0,   0,   473, 483, 0,
    452, 0,   452, 452, 0,   0,   0,   0,   0,   0,   532, 0,   0,   0,   0,   0,   612, 0,   0,
    0,   0,   0,   0,   0,   426, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   99,  0,   655, 0,   0,   482, 478, 0,   487, 0,
    0,   451, 654, 486, 0,   656, 453, 0,   0,   0,   394, 0,   533, 0,   0,   0,   0,   0,   398,
    0,   0,   0,   400, 0,   0,   402, 542, 0,   0,   0,   403, 0,   0,   0,   0,   0,   589, 549,
    552, 0,   0,   0,   405, 0,   406, 0,   0,   0,   657, 0,   474, 0,   475, 484, 0,   656, 0,
    0,   493, 496, 502, 505, 534, 535, 499, 514, 517, 538, 526, 529, 508, 401, 0,   0,   0,   539,
    523, 605, 606, 608, 553, 554, 555, 540, 511, 520, 333, 0,   488, 0,   452, 477, 0,   491, 0,
    543, 0,   0,   0,   476, 0,   473, 0,   455, 0,   0,   658, 0,   487, 454, 548, 547, 0,   475,
    0,   452, 489, 0,   0,   490};

const short ParserGen::yypgoto_[] = {
    -1040, 305,   19,    -1040, -1040, -9,    -1040, -1040, -5,    -1040, 3,     -1040, -717,
    295,   -1040, -1040, -158,  -1040, -1040, 0,     -63,   -42,   -38,   -30,   -20,   -28,
    -19,   -21,   -15,   -26,   -18,   -424,  -67,   -1040, -4,    6,     8,     -248,  10,
    18,    -52,   126,   -1040, -1040, -1040, -1040, -1040, -1040, -156,  -1040, 538,   -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, 200,   -866,  -545,  -1040, -14,
    442,   -448,  -1040, -1040, -65,   -60,   -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -407,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -297,  -1039, -169,  -820,  -666,  -1040, -1040, -393,  -403,  -380,  -1040, -1040, -1040,
    -395,  -1040, -597,  -1040, -171,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, -1040, -1040, -1040, -367,  -54,   107,   186,   254,   -396,  -1040, 20,
    -1040, -1040, -1040, -1040, -139,  -1040, -1040, -1040, -1040, -1040, -1040, -1040, -1040,
    698,   -422,  -1040, -1040, -1040, -1040, -1040, 246,   -1040, -1040, -1040, -1040, -1040,
    -1040, -1040, 199};

const short ParserGen::yydefgoto_[] = {
    -1,   912, 569, 923,  193,  194,  82,   195, 196,  197, 198,  199,  562,  200, 71,   570,  914,
    927,  577, 83,  259,  260,  261,  262,  263, 264,  265, 266,  267,  268,  269, 270,  271,  272,
    273,  274, 275, 276,  277,  278,  279,  589, 281,  282, 283,  456,  284,  765, 766,  7,    16,
    26,   27,  28,  29,   30,   31,   32,   451, 915,  751, 752,  323,  754,  767, 590,  614,  921,
    591,  592, 593, 770,  325,  326,  327,  328, 329,  330, 331,  332,  333,  334, 335,  336,  337,
    338,  339, 340, 341,  342,  343,  344,  693, 345,  346, 347,  348,  349,  350, 351,  352,  353,
    354,  355, 356, 357,  358,  359,  360,  361, 362,  363, 364,  365,  366,  367, 368,  369,  370,
    371,  372, 373, 374,  375,  376,  377,  378, 379,  380, 381,  382,  383,  384, 385,  386,  387,
    388,  389, 390, 391,  392,  393,  394,  395, 396,  397, 398,  399,  400,  401, 402,  403,  404,
    405,  406, 407, 1000, 1058, 1007, 1012, 835, 1034, 937, 1062, 1154, 1004, 793, 1064, 1009, 1116,
    1005, 459, 455, 1018, 408,  409,  410,  411, 412,  413, 414,  415,  416,  417, 418,  419,  420,
    421,  422, 423, 424,  425,  426,  427,  428, 429,  430, 431,  600,  601,  594, 595,  603,  604,
    631,  9,   17,  457,  287,  458,  73,   74,  582,  583, 584,  585,  75,   76,  918,  11,   18,
    433,  434, 435, 436,  437,  563,  84,   564, 13,   19,  448,  449,  749,  201, 5,    694};

const short ParserGen::yytable_[] = {
    221,  219,  220,  221,  219,  220,  222,  223,  68,   222,  226,  316,  69,   324,  316,  309,
    324,  72,   309,  72,   70,   787,  753,  753,  432,  617,  322,  432,  579,  322,  1121, 574,
    761,  913,  868,  747,  310,  81,   6,    310,  311,  642,  8,    311,  645,  646,  982,  983,
    312,  10,   313,  312,  314,  313,  12,   314,  14,   665,  666,  15,   315,  33,   586,  315,
    580,  288,  688,  202,  902,  251,  206,  207,  208,  578,  317,  748,  579,  317,  224,  704,
    1156, 225,  707,  227,  318,  229,  319,  318,  320,  319,  450,  320,  641,  452,  602,  602,
    321,  230,  231,  321,  602,  602,  602,  453,  602,  1,    2,    3,    4,    565,  602,  602,
    580,  695,  696,  757,  629,  629,  629,  602,  758,  581,  1013, 1014, 303,  1016, 771,  629,
    1020, 1021, 1022, 629,  629,  629,  789,  1026, 1027, 792,  1029, 743,  744,  768,  629,  629,
    794,  629,  795,  1039, 607,  796,  773,  602,  759,  609,  613,  760,  618,  774,  619,  629,
    1052, 1053, 1002, -481, -481, 1003, 602,  602,  640,  581,  20,   21,   22,   23,   24,   25,
    799,  602,  602,  203,  204,  205,  650,  651,  206,  207,  208,  1066, 280,  1068, 1069, 217,
    629,  629,  661,  788,  663,  286,  913,  683,  755,  755,  687,  209,  210,  211,  689,  691,
    699,  705,  706,  708,  713,  212,  213,  214,  715,  718,  719,  720,  721,  735,  775,  777,
    228,  779,  780,  781,  783,  784,  34,   785,  35,   36,   37,   38,   39,   828,  40,   41,
    42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,
    58,   59,   60,   786,  709,  790,  61,   35,   36,   37,   38,   39,   62,   40,   41,   42,
    43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   791,  63,   797,  61,   798,  800,  801,  802,  803,  804,  805,  806,  64,   215,
    216,  217,  218,  808,  809,  810,  811,  812,  813,  814,  815,  816,  819,  820,  821,  822,
    823,  920,  77,   561,  824,  825,  826,  827,  829,  65,   831,  66,   832,  78,   1164, 833,
    834,  836,  837,  838,  203,  204,  205,  839,  847,  206,  207,  208,  840,  850,  232,  233,
    841,  842,  854,  843,  844,  862,  234,  1181, 845,  846,  1030, 848,  209,  210,  211,  753,
    753,  849,  852,  1040, 1041, 1042, 212,  213,  214,  853,  856,  439,  440,  865,  858,  860,
    441,  442,  572,  235,  236,  861,  67,   863,  864,  866,  869,  871,  237,  238,  606,  870,
    608,  599,  599,  443,  444,  239,  907,  599,  599,  599,  872,  599,  875,  445,  446,  878,
    879,  599,  599,  880,  882,  79,   80,   884,  885,  890,  599,  242,  891,  892,  893,  221,
    219,  220,  910,  894,  895,  222,  571,  896,  897,  898,  899,  900,  901,  904,  243,  905,
    906,  908,  909,  690,  919,  911,  928,  929,  72,   930,  599,  933,  951,  447,  936,  960,
    215,  216,  217,  218,  954,  966,  978,  986,  991,  599,  599,  992,  994,  995,  996,  999,
    997,  998,  1006, 1008, 599,  599,  1015, 1024, 1011, 1023, 1031, 1028, 1032, 1033, 316,  316,
    324,  324,  309,  309,  316,  316,  324,  324,  309,  309,  1035, 432,  432,  322,  322,  221,
    219,  220,  764,  322,  322,  222,  710,  310,  310,  1036, 1037, 311,  311,  310,  310,  1043,
    762,  311,  311,  312,  312,  313,  313,  314,  314,  312,  312,  313,  313,  314,  314,  315,
    315,  755,  755,  1044, 1045, 315,  315,  1049, 1051, 1057, 1054, 1061, 1063, 317,  317,  1071,
    221,  219,  220,  317,  317,  1072, 222,  318,  318,  319,  319,  320,  320,  318,  318,  319,
    319,  320,  320,  321,  321,  280,  561,  1073, 1074, 321,  321,  598,  598,  1075, 630,  633,
    636,  598,  598,  598,  1077, 598,  1079, 1078, 1091, 647,  1080, 598,  598,  652,  655,  658,
    1081, 628,  628,  628,  598,  1082, 1083, 1084, 677,  680,  1085, 684,  628,  1086, 1087, 1089,
    628,  628,  628,  1094, 1095, 1096, 1100, 1101, 1102, 701,  1103, 628,  628,  1106, 628,  1104,
    1105, 1113, 1107, 1108, 598,  1109, 1110, 1115, 605,  1118, 1120, 1122, 628,  610,  611,  612,
    1123, 615,  1124, 598,  598,  1125, 1126, 621,  622,  1127, 737,  740,  1128, 1129, 598,  598,
    639,  1130, 1137, 620,  1131, 1132, 623,  624,  625,  626,  632,  635,  638,  628,  628,  1133,
    221,  219,  220,  1134, 1135, 649,  222,  877,  1138, 654,  657,  660,  1139, 1140, 280,  1141,
    692,  280,  667,  1142, 679,  682,  1143, 686,  1144, 1145, 1146, 1147, 1148, 1149, 1150, 711,
    712,  697,  698,  1153, 700,  703,  1157, 1158, 1159, 1162, 722,  723,  1003, 1171, 1166, 1167,
    1168, 916,  634,  637,  1169, 1170, 1173, 1174, 1175, 280,  1178, 1180, 648,  1182, 1183, 576,
    653,  656,  659,  734,  568,  985,  739,  742,  1067, 924,  989,  678,  681,  925,  685,  454,
    756,  1119, 72,   1010, 1172, 926,  1179, 1165, 1019, 1177, 987,  438,  702,  917,  35,   36,
    37,   38,   39,   280,  40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,
    52,   53,   54,   55,   56,   57,   58,   59,   60,   746,  0,    0,    61,   738,  741,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    566,  0,    0,    0,    316,  316,  324,
    324,  309,  309,  764,  567,  0,    0,    0,    221,  219,  220,  0,    322,  322,  222,  971,
    0,    0,    0,    0,    0,    0,    310,  310,  0,    0,    311,  311,  0,    0,    0,    0,
    0,    0,    312,  312,  313,  313,  314,  314,  0,    0,    0,    0,    0,    0,    315,  315,
    0,    221,  219,  220,  0,    0,    596,  222,  0,    0,    0,    0,    317,  317,  0,    0,
    0,    0,    616,  0,    0,    0,    318,  318,  319,  319,  320,  320,  0,    0,    0,    0,
    0,    0,    321,  321,  643,  644,  0,    0,    0,    0,    0,    0,    79,   80,   0,    0,
    662,  0,    664,  0,    0,    35,   36,   37,   38,   39,   0,    40,   41,   42,   43,   44,
    45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    668,  669,  714,  61,   716,  717,  0,    0,    670,  0,    0,    0,    724,  725,  726,  727,
    728,  729,  730,  731,  732,  733,  0,    0,    736,  0,    0,    0,    0,    0,    0,    0,
    745,  0,    203,  204,  205,  671,  672,  206,  207,  208,  0,    495,  0,    0,    673,  674,
    0,    232,  233,  0,    0,    0,    0,    675,  0,    234,  209,  210,  211,  0,    0,    772,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    776,  0,    778,  0,    676,  0,    782,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    807,  0,    0,    0,    0,    0,
    0,    0,    0,    763,  817,  818,  0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    830,  0,    79,   80,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  0,    0,    0,    851,  0,    0,    0,    855,  0,    857,  0,    859,  215,  216,
    217,  218,  0,    0,    0,    867,  0,    0,    0,    0,    0,    873,  874,  0,    876,  0,
    0,    0,    0,    881,  0,    883,  0,    0,    886,  887,  888,  889,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    903,  0,    0,    35,   36,   37,   38,
    39,   0,    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,
    54,   55,   56,   57,   58,   59,   60,   0,    0,    0,    61,   0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    931,  0,    0,    0,    0,    0,    0,    934,
    0,    0,    935,  0,    938,  939,  940,  984,  0,    941,  0,    0,    942,  0,    0,    943,
    0,    944,  495,  0,    0,    0,    0,    0,    0,    0,    945,  946,  947,  0,    0,    948,
    0,    0,    949,  0,    0,    950,  0,    0,    952,  0,    0,    0,    953,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    955,  0,    0,    956,  957,  0,    0,
    958,  959,  0,    0,    0,    961,  0,    0,    962,  0,    0,    963,  964,  965,  0,    0,
    0,    967,  0,    968,  969,  0,    970,  0,    0,    0,    0,    972,  0,    973,  0,    0,
    974,  975,  976,  977,  0,    0,    0,    0,    0,    0,    79,   80,   0,    0,    0,    0,
    0,    979,  0,    0,    0,    980,  0,    0,    981,  0,    0,    0,    0,    0,    0,    0,
    0,    990,  0,    0,    0,    993,  0,    0,    0,    203,  204,  205,  0,    0,    206,  207,
    208,  988,  1001, 0,    0,    0,    0,    0,    232,  233,  0,    0,    1017, 1017, 0,    0,
    234,  209,  210,  211,  1025, 0,    0,    0,    0,    0,    0,    212,  213,  214,  0,    0,
    1038, 0,    0,    0,    0,    0,    0,    0,    1046, 1047, 1048, 0,    1050, 235,  236,  0,
    0,    0,    0,    0,    0,    0,    237,  238,  1055, 0,    0,    0,    0,    0,    0,    239,
    0,    1056, 0,    0,    1059, 1060, 0,    0,    1065, 0,    0,    0,    0,    1070, 0,    0,
    0,    0,    0,    1076, 0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    1090, 0,    1092, 1093, 0,    243,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    215,  216,  217,  218,  1111, 0,    1112, 0,    0,    0,    1114,
    0,    1117, 35,   36,   37,   38,   39,   0,    40,   41,   42,   43,   44,   45,   46,   47,
    48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   1136, 0,    0,
    61,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    1151, 0,    1152, 0,    0,    1155, 0,    0,    0,    0,    922,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    64,   0,    0,    1160, 1161, 0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1163, 0,    0,    0,    0,
    0,    85,   86,   87,   88,   89,   90,   91,   35,   36,   37,   38,   39,   1176, 40,   41,
    42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,
    58,   59,   60,   92,   93,   94,   61,   95,   96,   0,    0,    97,   0,    98,   99,   100,
    101,  102,  103,  104,  105,  106,  107,  108,  109,  110,  0,    0,    0,    111,  112,  0,
    67,   0,    0,    113,  114,  115,  0,    116,  117,  0,    0,    118,  119,  120,  64,   121,
    122,  0,    0,    0,    0,    123,  124,  125,  126,  127,  128,  129,  0,    0,    0,    130,
    131,  132,  133,  134,  135,  136,  137,  138,  139,  0,    140,  141,  142,  143,  0,    0,
    144,  145,  146,  147,  148,  149,  150,  0,    0,    151,  152,  153,  154,  155,  156,  157,
    0,    158,  159,  160,  161,  162,  163,  164,  165,  166,  167,  0,    0,    168,  169,  170,
    171,  172,  173,  174,  175,  176,  0,    177,  178,  179,  180,  181,  182,  183,  184,  185,
    186,  187,  188,  189,  190,  191,  67,   192,  460,  461,  462,  463,  464,  465,  466,  35,
    36,   37,   38,   39,   0,    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,
    51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   467,  468,  469,  61,   470,  471,
    0,    0,    472,  0,    473,  474,  475,  476,  477,  478,  479,  480,  481,  482,  483,  484,
    485,  0,    0,    0,    486,  487,  0,    0,    0,    0,    0,    488,  489,  0,    490,  491,
    0,    0,    492,  493,  494,  495,  496,  497,  0,    0,    0,    0,    498,  499,  500,  501,
    502,  503,  504,  0,    0,    0,    505,  506,  507,  508,  509,  510,  511,  512,  513,  514,
    0,    515,  516,  517,  518,  0,    0,    519,  520,  521,  522,  523,  524,  525,  0,    0,
    526,  527,  528,  529,  530,  531,  532,  0,    533,  534,  535,  536,  0,    0,    0,    0,
    0,    0,    0,    0,    537,  538,  539,  540,  541,  542,  543,  544,  545,  0,    546,  547,
    548,  549,  550,  551,  552,  553,  554,  555,  556,  557,  558,  559,  560,  79,   80,   460,
    461,  462,  463,  464,  465,  466,  35,   36,   37,   38,   39,   0,    40,   41,   42,   43,
    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,
    60,   467,  468,  469,  61,   470,  471,  0,    0,    472,  0,    473,  474,  475,  476,  477,
    478,  479,  480,  481,  482,  483,  484,  485,  0,    0,    0,    486,  487,  0,    0,    0,
    0,    0,    0,    489,  0,    490,  491,  0,    0,    492,  493,  494,  495,  496,  497,  0,
    0,    0,    0,    498,  499,  500,  501,  502,  503,  504,  0,    0,    0,    505,  506,  507,
    508,  509,  510,  511,  512,  513,  514,  0,    515,  516,  517,  518,  0,    0,    519,  520,
    521,  522,  523,  524,  525,  0,    0,    526,  527,  528,  529,  530,  531,  769,  0,    533,
    534,  535,  536,  0,    0,    0,    0,    0,    0,    0,    0,    537,  538,  539,  540,  541,
    542,  543,  544,  545,  0,    546,  547,  548,  549,  550,  551,  552,  553,  554,  555,  556,
    557,  558,  559,  560,  79,   80,   85,   86,   87,   88,   89,   90,   91,   35,   36,   37,
    38,   39,   0,    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,
    53,   54,   55,   56,   57,   58,   59,   60,   92,   93,   94,   61,   95,   96,   0,    0,
    97,   0,    98,   99,   100,  101,  102,  103,  104,  105,  106,  107,  108,  109,  110,  0,
    0,    0,    111,  112,  0,    0,    0,    0,    575,  114,  115,  0,    116,  117,  0,    0,
    118,  119,  120,  64,   121,  122,  0,    0,    0,    0,    123,  124,  125,  126,  127,  128,
    129,  0,    0,    0,    130,  131,  132,  133,  134,  135,  136,  137,  138,  139,  0,    140,
    141,  142,  143,  0,    0,    144,  145,  146,  147,  148,  149,  150,  0,    0,    151,  152,
    153,  154,  155,  156,  157,  0,    158,  159,  160,  161,  162,  163,  164,  165,  166,  167,
    0,    0,    168,  169,  170,  171,  172,  173,  174,  175,  176,  0,    177,  178,  179,  180,
    181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  67,   460,  461,  462,  463,
    464,  465,  466,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    467,  468,
    469,  0,    470,  471,  0,    0,    472,  0,    473,  474,  475,  476,  477,  478,  479,  480,
    481,  482,  483,  484,  485,  0,    0,    0,    486,  487,  0,    0,    0,    0,    0,    0,
    489,  0,    490,  491,  0,    0,    492,  493,  494,  0,    496,  497,  0,    0,    0,    0,
    498,  499,  500,  501,  502,  503,  504,  0,    0,    0,    505,  506,  507,  508,  509,  510,
    511,  512,  513,  514,  0,    515,  516,  517,  518,  0,    0,    519,  520,  521,  522,  523,
    524,  525,  0,    0,    526,  527,  528,  529,  530,  531,  769,  0,    533,  534,  535,  536,
    0,    0,    0,    0,    0,    0,    0,    0,    537,  538,  539,  540,  541,  542,  543,  544,
    545,  0,    546,  547,  548,  549,  550,  551,  552,  553,  554,  555,  556,  557,  558,  559,
    560,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  573,  0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    240,  241,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  587,  0,
    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,
    211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,
    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,
    254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,
    207,  208,  932,  0,    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,
    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  1088, 0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,
    588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  1097, 0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  1098, 0,
    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,
    211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,
    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,
    254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,
    207,  208,  1099, 0,    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,
    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  996,  0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,
    588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  0,    0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    240,  241,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  0,    0,
    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,
    211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,
    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,
    254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,
    207,  208,  0,    0,    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,
    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    597,  588,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  0,    0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    597,
    627,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  0,    0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    285,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  289,  290,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    291,  292,  293,  0,    0,    294,  295,  296,  0,    0,
    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  297,  298,
    299,  0,    0,    0,    0,    0,    0,    0,    300,  301,  302,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,
    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    303,  304,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    243,  0,    0,    246,  247,  248,  249,  250,  251,  252,  253,
    254,  255,  305,  306,  307,  308,  256,  257,  258,  289,  290,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    291,  292,  293,  0,    0,    294,
    295,  296,  0,    0,    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,
    0,    234,  297,  298,  299,  0,    0,    0,    0,    0,    0,    0,    300,  301,  302,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,  750,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  0,    0,    246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  305,  306,  307,  308,  256,  257,  258};

const short ParserGen::yycheck_[] = {
    21,   21,   21,  24,   24,   24,   21,   21,   17,   24,   24,  78,   17,   78,   81,   78,
    81,   17,   81,  19,   17,   618,  567,  568,  78,   473,  78,  81,   72,   81,   1069, 455,
    142,  750,  700, 112,  78,   18,   141,  81,   78,   489,  142, 81,   492,  493,  912,  913,
    78,   142,  78,  81,   78,   81,   142,  81,   0,    505,  506, 142,  78,   68,   458,  81,
    108,  141,  514, 142,  734,  179,  65,   66,   67,   69,   78,  152,  72,   81,   142,  527,
    1119, 142,  530, 142,  78,   69,   78,   81,   78,   81,   69,  81,   488,  34,   461,  462,
    78,   43,   44,  81,   467,  468,  469,  11,   471,  191,  192, 193,  194,  100,  477,  478,
    108,  520,  521, 69,   483,  484,  485,  486,  30,   165,  942, 943,  141,  945,  69,   494,
    948,  949,  950, 498,  499,  500,  19,   955,  956,  14,   958, 561,  562,  68,   509,  510,
    13,   512,  12,  967,  141,  12,   69,   518,  576,  141,  141, 579,  141,  69,   141,  526,
    980,  981,  21,  22,   23,   24,   533,  534,  141,  165,  135, 136,  137,  138,  139,  140,
    12,   544,  545, 60,   61,   62,   141,  141,  65,   66,   67,  1007, 62,   1009, 1010, 186,
    559,  560,  141, 619,  141,  71,   915,  141,  567,  568,  141, 84,   85,   86,   141,  141,
    141,  141,  141, 141,  141,  94,   95,   96,   141,  141,  141, 141,  141,  141,  69,   69,
    25,   69,   69,  69,   69,   69,   8,    69,   10,   11,   12,  13,   14,   661,  16,   17,
    18,   19,   20,  21,   22,   23,   24,   25,   26,   27,   28,  29,   30,   31,   32,   33,
    34,   35,   36,  69,   141,  69,   40,   10,   11,   12,   13,  14,   46,   16,   17,   18,
    19,   20,   21,  22,   23,   24,   25,   26,   27,   28,   29,  30,   31,   32,   33,   34,
    35,   36,   69,  69,   69,   40,   69,   69,   69,   12,   69,  69,   12,   69,   80,   184,
    185,  186,  187, 69,   69,   69,   69,   69,   69,   69,   69,  12,   69,   69,   12,   69,
    69,   769,  69,  304,  12,   69,   69,   12,   69,   107,  69,  109,  69,   80,   1154, 69,
    10,   69,   69,  69,   60,   61,   62,   69,   12,   65,   66,  67,   69,   12,   75,   76,
    69,   69,   12,  69,   69,   19,   83,   1179, 69,   69,   959, 69,   84,   85,   86,   912,
    913,  69,   69,  968,  969,  970,  94,   95,   96,   69,   69,  60,   61,   16,   69,   69,
    65,   66,   453, 112,  113,  69,   168,  69,   69,   16,   69,  12,   121,  122,  463,  69,
    465,  461,  462, 84,   85,   130,  12,   467,  468,  469,  69,  471,  69,   94,   95,   69,
    69,   477,  478, 69,   69,   168,  169,  69,   69,   69,   486, 152,  69,   69,   69,   452,
    452,  452,  12,  69,   69,   452,  452,  69,   69,   69,   69,  69,   69,   69,   171,  69,
    69,   69,   69,  516,  141,  69,   68,   68,   456,  68,   518, 68,   68,   142,  18,   68,
    184,  185,  186, 187,  19,   19,   19,   69,   69,   533,  534, 69,   69,   69,   68,   27,
    69,   69,   17,  20,   544,  545,  68,   68,   35,   69,   69,  68,   68,   29,   561,  562,
    561,  562,  561, 562,  567,  568,  567,  568,  567,  568,  19,  561,  562,  561,  562,  532,
    532,  532,  581, 567,  568,  532,  532,  561,  562,  19,   68,  561,  562,  567,  568,  68,
    580,  567,  568, 561,  562,  561,  562,  561,  562,  567,  568, 567,  568,  567,  568,  561,
    562,  912,  913, 68,   68,   567,  568,  68,   68,   28,   69,  25,   22,   561,  562,  69,
    581,  581,  581, 567,  568,  69,   581,  561,  562,  561,  562, 561,  562,  567,  568,  567,
    568,  567,  568, 561,  562,  455,  563,  69,   69,   567,  568, 461,  462,  69,   483,  484,
    485,  467,  468, 469,  68,   471,  69,   68,   31,   494,  69,  477,  478,  498,  499,  500,
    69,   483,  484, 485,  486,  69,   69,   69,   509,  510,  69,  512,  494,  69,   69,   69,
    498,  499,  500, 69,   69,   69,   69,   69,   69,   526,  68,  509,  510,  69,   512,  68,
    68,   36,   69,  69,   518,  69,   69,   26,   462,  23,   69,  69,   526,  467,  468,  469,
    69,   471,  69,  533,  534,  69,   69,   477,  478,  69,   559, 560,  69,   69,   544,  545,
    486,  69,   32,  476,  69,   69,   479,  480,  481,  482,  483, 484,  485,  559,  560,  69,
    709,  709,  709, 69,   69,   494,  709,  709,  32,   498,  499, 500,  69,   69,   576,  69,
    518,  579,  507, 69,   509,  510,  69,   512,  69,   69,   69,  69,   69,   69,   68,   533,
    534,  522,  523, 33,   525,  526,  69,   69,   69,   69,   544, 545,  24,   40,   69,   69,
    69,   758,  484, 485,  69,   69,   69,   69,   69,   619,  69,  69,   494,  69,   69,   456,
    498,  499,  500, 556,  451,  915,  559,  560,  1008, 770,  918, 509,  510,  770,  512,  229,
    568,  1066, 770, 940,  1165, 770,  1177, 1155, 947,  1172, 917, 81,   526,  761,  10,   11,
    12,   13,   14,  661,  16,   17,   18,   19,   20,   21,   22,  23,   24,   25,   26,   27,
    28,   29,   30,  31,   32,   33,   34,   35,   36,   563,  -1,  -1,   40,   559,  560,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   69,   -1,  -1,   -1,   912,  913,  912,
    913,  912,  913, 918,  80,   -1,   -1,   -1,   877,  877,  877, -1,   912,  913,  877,  877,
    -1,   -1,   -1,  -1,   -1,   -1,   912,  913,  -1,   -1,   912, 913,  -1,   -1,   -1,   -1,
    -1,   -1,   912, 913,  912,  913,  912,  913,  -1,   -1,   -1,  -1,   -1,   -1,   912,  913,
    -1,   918,  918, 918,  -1,   -1,   460,  918,  -1,   -1,   -1,  -1,   912,  913,  -1,   -1,
    -1,   -1,   472, -1,   -1,   -1,   912,  913,  912,  913,  912, 913,  -1,   -1,   -1,   -1,
    -1,   -1,   912, 913,  490,  491,  -1,   -1,   -1,   -1,   -1,  -1,   168,  169,  -1,   -1,
    502,  -1,   504, -1,   -1,   10,   11,   12,   13,   14,   -1,  16,   17,   18,   19,   20,
    21,   22,   23,  24,   25,   26,   27,   28,   29,   30,   31,  32,   33,   34,   35,   36,
    75,   76,   536, 40,   538,  539,  -1,   -1,   83,   -1,   -1,  -1,   546,  547,  548,  549,
    550,  551,  552, 553,  554,  555,  -1,   -1,   558,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    69,   -1,   60,  61,   62,   112,  113,  65,   66,   67,   -1,  80,   -1,   -1,   121,  122,
    -1,   75,   76,  -1,   -1,   -1,   -1,   130,  -1,   83,   84,  85,   86,   -1,   -1,   597,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   607,  -1,  609,  -1,   152,  -1,   613,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   640, -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  650,  651,  -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,
    -1,   663,  -1,  168,  169,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  -1,  -1,   -1,   683,  -1,   -1,   -1,   687,  -1,  689,  -1,   691,  184,  185,
    186,  187,  -1,  -1,   -1,   699,  -1,   -1,   -1,   -1,   -1,  705,  706,  -1,   708,  -1,
    -1,   -1,   -1,  713,  -1,   715,  -1,   -1,   718,  719,  720, 721,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   735,  -1,  -1,   10,   11,   12,   13,
    14,   -1,   16,  17,   18,   19,   20,   21,   22,   23,   24,  25,   26,   27,   28,   29,
    30,   31,   32,  33,   34,   35,   36,   -1,   -1,   -1,   40,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   782,  -1,   -1,  -1,   -1,   -1,   -1,   789,
    -1,   -1,   792, -1,   794,  795,  796,  69,   -1,   799,  -1,  -1,   802,  -1,   -1,   805,
    -1,   807,  80,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   816, 817,  818,  -1,   -1,   821,
    -1,   -1,   824, -1,   -1,   827,  -1,   -1,   830,  -1,   -1,  -1,   834,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   847,  -1,  -1,   850,  851,  -1,   -1,
    854,  855,  -1,  -1,   -1,   859,  -1,   -1,   862,  -1,   -1,  865,  866,  867,  -1,   -1,
    -1,   871,  -1,  873,  874,  -1,   876,  -1,   -1,   -1,   -1,  881,  -1,   883,  -1,   -1,
    886,  887,  888, 889,  -1,   -1,   -1,   -1,   -1,   -1,   168, 169,  -1,   -1,   -1,   -1,
    -1,   903,  -1,  -1,   -1,   907,  -1,   -1,   910,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   919,  -1,  -1,   -1,   923,  -1,   -1,   -1,   60,   61,  62,   -1,   -1,   65,   66,
    67,   68,   936, -1,   -1,   -1,   -1,   -1,   75,   76,   -1,  -1,   946,  947,  -1,   -1,
    83,   84,   85,  86,   954,  -1,   -1,   -1,   -1,   -1,   -1,  94,   95,   96,   -1,   -1,
    966,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   974,  975,  976, -1,   978,  112,  113,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   121,  122,  990,  -1,   -1,  -1,   -1,   -1,   -1,   130,
    -1,   999,  -1,  -1,   1002, 1003, -1,   -1,   1006, -1,   -1,  -1,   -1,   1011, -1,   -1,
    -1,   -1,   -1,  1017, -1,   152,  -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  1033, -1,   1035, 1036, -1,   171,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   184,  185,  186,  187,  1055, -1,  1057, -1,   -1,   -1,   1061,
    -1,   1063, 10,  11,   12,   13,   14,   -1,   16,   17,   18,  19,   20,   21,   22,   23,
    24,   25,   26,  27,   28,   29,   30,   31,   32,   33,   34,  35,   36,   1091, -1,   -1,
    40,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  1113, -1,   1115, -1,   -1,   1118, -1,   -1,  -1,   -1,   69,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   80,   -1,   -1,  1137, 1138, -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  1153, -1,   -1,   -1,   -1,
    -1,   3,    4,   5,    6,    7,    8,    9,    10,   11,   12,  13,   14,   1171, 16,   17,
    18,   19,   20,  21,   22,   23,   24,   25,   26,   27,   28,  29,   30,   31,   32,   33,
    34,   35,   36,  37,   38,   39,   40,   41,   42,   -1,   -1,  45,   -1,   47,   48,   49,
    50,   51,   52,  53,   54,   55,   56,   57,   58,   59,   -1,  -1,   -1,   63,   64,   -1,
    168,  -1,   -1,  69,   70,   71,   -1,   73,   74,   -1,   -1,  77,   78,   79,   80,   81,
    82,   -1,   -1,  -1,   -1,   87,   88,   89,   90,   91,   92,  93,   -1,   -1,   -1,   97,
    98,   99,   100, 101,  102,  103,  104,  105,  106,  -1,   108, 109,  110,  111,  -1,   -1,
    114,  115,  116, 117,  118,  119,  120,  -1,   -1,   123,  124, 125,  126,  127,  128,  129,
    -1,   131,  132, 133,  134,  135,  136,  137,  138,  139,  140, -1,   -1,   143,  144,  145,
    146,  147,  148, 149,  150,  151,  -1,   153,  154,  155,  156, 157,  158,  159,  160,  161,
    162,  163,  164, 165,  166,  167,  168,  169,  3,    4,    5,   6,    7,    8,    9,    10,
    11,   12,   13,  14,   -1,   16,   17,   18,   19,   20,   21,  22,   23,   24,   25,   26,
    27,   28,   29,  30,   31,   32,   33,   34,   35,   36,   37,  38,   39,   40,   41,   42,
    -1,   -1,   45,  -1,   47,   48,   49,   50,   51,   52,   53,  54,   55,   56,   57,   58,
    59,   -1,   -1,  -1,   63,   64,   -1,   -1,   -1,   -1,   -1,  70,   71,   -1,   73,   74,
    -1,   -1,   77,  78,   79,   80,   81,   82,   -1,   -1,   -1,  -1,   87,   88,   89,   90,
    91,   92,   93,  -1,   -1,   -1,   97,   98,   99,   100,  101, 102,  103,  104,  105,  106,
    -1,   108,  109, 110,  111,  -1,   -1,   114,  115,  116,  117, 118,  119,  120,  -1,   -1,
    123,  124,  125, 126,  127,  128,  129,  -1,   131,  132,  133, 134,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   143,  144,  145,  146,  147,  148,  149, 150,  151,  -1,   153,  154,
    155,  156,  157, 158,  159,  160,  161,  162,  163,  164,  165, 166,  167,  168,  169,  3,
    4,    5,    6,   7,    8,    9,    10,   11,   12,   13,   14,  -1,   16,   17,   18,   19,
    20,   21,   22,  23,   24,   25,   26,   27,   28,   29,   30,  31,   32,   33,   34,   35,
    36,   37,   38,  39,   40,   41,   42,   -1,   -1,   45,   -1,  47,   48,   49,   50,   51,
    52,   53,   54,  55,   56,   57,   58,   59,   -1,   -1,   -1,  63,   64,   -1,   -1,   -1,
    -1,   -1,   -1,  71,   -1,   73,   74,   -1,   -1,   77,   78,  79,   80,   81,   82,   -1,
    -1,   -1,   -1,  87,   88,   89,   90,   91,   92,   93,   -1,  -1,   -1,   97,   98,   99,
    100,  101,  102, 103,  104,  105,  106,  -1,   108,  109,  110, 111,  -1,   -1,   114,  115,
    116,  117,  118, 119,  120,  -1,   -1,   123,  124,  125,  126, 127,  128,  129,  -1,   131,
    132,  133,  134, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  143,  144,  145,  146,  147,
    148,  149,  150, 151,  -1,   153,  154,  155,  156,  157,  158, 159,  160,  161,  162,  163,
    164,  165,  166, 167,  168,  169,  3,    4,    5,    6,    7,   8,    9,    10,   11,   12,
    13,   14,   -1,  16,   17,   18,   19,   20,   21,   22,   23,  24,   25,   26,   27,   28,
    29,   30,   31,  32,   33,   34,   35,   36,   37,   38,   39,  40,   41,   42,   -1,   -1,
    45,   -1,   47,  48,   49,   50,   51,   52,   53,   54,   55,  56,   57,   58,   59,   -1,
    -1,   -1,   63,  64,   -1,   -1,   -1,   -1,   69,   70,   71,  -1,   73,   74,   -1,   -1,
    77,   78,   79,  80,   81,   82,   -1,   -1,   -1,   -1,   87,  88,   89,   90,   91,   92,
    93,   -1,   -1,  -1,   97,   98,   99,   100,  101,  102,  103, 104,  105,  106,  -1,   108,
    109,  110,  111, -1,   -1,   114,  115,  116,  117,  118,  119, 120,  -1,   -1,   123,  124,
    125,  126,  127, 128,  129,  -1,   131,  132,  133,  134,  135, 136,  137,  138,  139,  140,
    -1,   -1,   143, 144,  145,  146,  147,  148,  149,  150,  151, -1,   153,  154,  155,  156,
    157,  158,  159, 160,  161,  162,  163,  164,  165,  166,  167, 168,  3,    4,    5,    6,
    7,    8,    9,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   37,   38,
    39,   -1,   41,  42,   -1,   -1,   45,   -1,   47,   48,   49,  50,   51,   52,   53,   54,
    55,   56,   57,  58,   59,   -1,   -1,   -1,   63,   64,   -1,  -1,   -1,   -1,   -1,   -1,
    71,   -1,   73,  74,   -1,   -1,   77,   78,   79,   -1,   81,  82,   -1,   -1,   -1,   -1,
    87,   88,   89,  90,   91,   92,   93,   -1,   -1,   -1,   97,  98,   99,   100,  101,  102,
    103,  104,  105, 106,  -1,   108,  109,  110,  111,  -1,   -1,  114,  115,  116,  117,  118,
    119,  120,  -1,  -1,   123,  124,  125,  126,  127,  128,  129, -1,   131,  132,  133,  134,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   143,  144,  145, 146,  147,  148,  149,  150,
    151,  -1,   153, 154,  155,  156,  157,  158,  159,  160,  161, 162,  163,  164,  165,  166,
    167,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   68,  -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,  85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180, 181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,  65,   66,   67,   68,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,  -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112, 113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176, 177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,  61,   62,   -1,   -1,   65,
    66,   67,   68,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,  -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172, 173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188, 189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   68,   -1,   -1,   -1,   -1,  -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152, -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184, 185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   68,  -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,  85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180, 181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,  65,   66,   67,   68,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,  -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112, 113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176, 177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,  61,   62,   -1,   -1,   65,
    66,   67,   68,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,  -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172, 173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188, 189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   68,   -1,   -1,   -1,   -1,  -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152, -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184, 185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,  85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180, 181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,  65,   66,   67,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,  -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112, 113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176, 177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,  61,   62,   -1,   -1,   65,
    66,   67,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,  -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172, 173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188, 189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152, -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184, 185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,  85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   142,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180, 181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,  65,   66,   67,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,  -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112, 113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  -1,   -1,   174,  175,  176, 177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,  61,   62,   -1,   -1,   65,
    66,   67,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,  -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  -1,  -1,   174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188, 189,  190};

const short ParserGen::yystos_[] = {
    0,   191, 192, 193, 194, 430, 141, 244, 142, 400, 142, 414, 142, 424, 0,   142, 245, 401, 415,
    425, 135, 136, 137, 138, 139, 140, 246, 247, 248, 249, 250, 251, 252, 68,  8,   10,  11,  12,
    13,  14,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  40,  46,  69,  80,  107, 109, 168, 200, 203, 205, 209, 214, 405, 406, 411,
    412, 69,  80,  168, 169, 197, 201, 214, 422, 3,   4,   5,   6,   7,   8,   9,   37,  38,  39,
    41,  42,  45,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  63,  64,  69,
    70,  71,  73,  74,  77,  78,  79,  81,  82,  87,  88,  89,  90,  91,  92,  93,  97,  98,  99,
    100, 101, 102, 103, 104, 105, 106, 108, 109, 110, 111, 114, 115, 116, 117, 118, 119, 120, 123,
    124, 125, 126, 127, 128, 129, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 143, 144, 145,
    146, 147, 148, 149, 150, 151, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165,
    166, 167, 169, 199, 200, 202, 203, 204, 205, 206, 208, 429, 142, 60,  61,  62,  65,  66,  67,
    84,  85,  86,  94,  95,  96,  184, 185, 186, 187, 219, 221, 222, 223, 259, 142, 142, 259, 142,
    431, 69,  43,  44,  75,  76,  83,  112, 113, 121, 122, 130, 141, 142, 152, 171, 172, 173, 174,
    175, 176, 177, 178, 179, 180, 181, 182, 183, 188, 189, 190, 215, 216, 217, 218, 219, 220, 221,
    222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 241,
    142, 236, 403, 141, 43,  44,  60,  61,  62,  65,  66,  67,  84,  85,  86,  94,  95,  96,  141,
    142, 184, 185, 186, 187, 215, 216, 217, 218, 220, 224, 225, 227, 229, 230, 231, 233, 234, 235,
    257, 264, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283,
    284, 285, 286, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322,
    323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341,
    342, 343, 344, 345, 346, 347, 348, 349, 350, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378,
    379, 380, 381, 382, 383, 384, 385, 386, 387, 388, 389, 390, 391, 392, 396, 416, 417, 418, 419,
    420, 416, 60,  61,  65,  66,  84,  85,  94,  95,  142, 426, 427, 69,  253, 34,  11,  245, 367,
    240, 402, 404, 366, 3,   4,   5,   6,   7,   8,   9,   37,  38,  39,  41,  42,  45,  47,  48,
    49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  63,  64,  70,  71,  73,  74,  77,  78,
    79,  80,  81,  82,  87,  88,  89,  90,  91,  92,  93,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 108, 109, 110, 111, 114, 115, 116, 117, 118, 119, 120, 123, 124, 125, 126, 127, 128,
    129, 131, 132, 133, 134, 143, 144, 145, 146, 147, 148, 149, 150, 151, 153, 154, 155, 156, 157,
    158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 197, 207, 421, 423, 100, 69,  80,  196, 197,
    210, 259, 227, 68,  226, 69,  208, 213, 69,  72,  108, 165, 407, 408, 409, 410, 400, 68,  142,
    236, 260, 263, 264, 265, 395, 396, 260, 141, 236, 265, 393, 394, 395, 397, 398, 398, 264, 141,
    264, 141, 398, 398, 398, 141, 261, 398, 260, 261, 141, 141, 431, 398, 398, 431, 431, 431, 431,
    142, 236, 395, 397, 399, 431, 397, 399, 431, 397, 399, 431, 398, 141, 400, 261, 260, 260, 261,
    261, 397, 399, 431, 141, 141, 397, 399, 431, 397, 399, 431, 397, 399, 431, 141, 260, 141, 260,
    261, 261, 431, 75,  76,  83,  112, 113, 121, 122, 130, 152, 397, 399, 431, 397, 399, 431, 141,
    397, 399, 431, 141, 261, 141, 264, 141, 398, 287, 431, 287, 287, 431, 431, 141, 431, 397, 399,
    431, 261, 141, 141, 261, 141, 141, 259, 398, 398, 141, 260, 141, 260, 260, 141, 141, 141, 141,
    398, 398, 260, 260, 260, 260, 260, 260, 260, 260, 260, 260, 431, 141, 260, 397, 399, 431, 397,
    399, 431, 417, 417, 69,  423, 112, 152, 428, 142, 255, 256, 257, 258, 395, 255, 69,  30,  226,
    226, 142, 235, 141, 227, 242, 243, 259, 68,  129, 266, 69,  260, 69,  69,  69,  260, 69,  260,
    69,  69,  69,  260, 69,  69,  69,  69,  366, 226, 19,  69,  69,  14,  361, 13,  12,  12,  69,
    69,  12,  69,  69,  12,  69,  69,  12,  69,  260, 69,  69,  69,  69,  69,  69,  69,  69,  12,
    260, 260, 69,  69,  12,  69,  69,  12,  69,  69,  12,  226, 69,  260, 69,  69,  69,  10,  355,
    69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  12,  69,  69,  12,  260, 69,  69,  12,
    260, 69,  260, 69,  260, 69,  69,  19,  69,  69,  16,  16,  260, 355, 69,  69,  12,  69,  260,
    260, 69,  260, 259, 69,  69,  69,  260, 69,  260, 69,  69,  260, 260, 260, 260, 69,  69,  69,
    69,  69,  69,  69,  69,  69,  69,  69,  69,  355, 260, 69,  69,  69,  12,  69,  69,  12,  69,
    196, 207, 211, 254, 222, 402, 413, 141, 261, 262, 69,  198, 200, 203, 205, 212, 68,  68,  68,
    260, 68,  68,  260, 260, 18,  357, 260, 260, 260, 260, 260, 260, 260, 260, 260, 260, 260, 260,
    260, 68,  260, 260, 19,  260, 260, 260, 260, 260, 68,  260, 260, 260, 260, 260, 19,  260, 260,
    260, 260, 259, 260, 260, 260, 260, 260, 260, 19,  260, 260, 260, 256, 256, 69,  211, 69,  407,
    68,  243, 260, 69,  69,  260, 69,  69,  68,  69,  69,  27,  351, 260, 21,  24,  360, 365, 17,
    353, 20,  363, 353, 35,  354, 354, 354, 68,  354, 260, 368, 368, 354, 354, 354, 69,  68,  260,
    354, 354, 68,  354, 366, 69,  68,  29,  356, 19,  19,  68,  260, 354, 366, 366, 366, 68,  68,
    68,  260, 260, 260, 68,  260, 68,  354, 354, 69,  260, 260, 28,  352, 260, 260, 25,  358, 22,
    362, 260, 354, 232, 354, 354, 260, 69,  69,  69,  69,  69,  260, 68,  68,  69,  69,  69,  69,
    69,  69,  69,  69,  69,  68,  69,  260, 31,  260, 260, 69,  69,  69,  68,  68,  68,  69,  69,
    69,  68,  68,  68,  69,  69,  69,  69,  69,  260, 260, 36,  260, 26,  364, 260, 23,  351, 69,
    352, 69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  260, 32,  32,  69,
    69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  68,  260, 260, 33,  359, 260, 352, 69,  69,
    69,  260, 260, 69,  260, 354, 360, 69,  69,  69,  69,  69,  40,  358, 69,  69,  69,  260, 364,
    69,  359, 69,  354, 69,  69};

const short ParserGen::yyr1_[] = {
    0,   195, 430, 430, 430, 430, 244, 245, 245, 431, 246, 246, 246, 246, 246, 246, 252, 247, 248,
    259, 259, 259, 259, 249, 250, 251, 253, 253, 210, 210, 255, 256, 256, 256, 257, 257, 257, 257,
    257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 257,
    257, 257, 257, 257, 257, 257, 257, 257, 257, 257, 196, 197, 197, 197, 258, 254, 254, 211, 211,
    400, 401, 401, 405, 405, 405, 403, 403, 402, 402, 407, 407, 407, 409, 242, 413, 413, 243, 243,
    410, 410, 411, 408, 408, 406, 412, 412, 412, 404, 404, 209, 209, 209, 203, 199, 199, 199, 199,
    199, 199, 200, 201, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214,
    214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 214, 202, 202, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 227, 227, 227,
    227, 227, 227, 227, 227, 227, 227, 228, 241, 229, 230, 231, 233, 234, 235, 215, 216, 217, 218,
    220, 224, 225, 219, 219, 219, 219, 221, 221, 221, 221, 222, 222, 222, 222, 223, 223, 223, 223,
    232, 232, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236, 236,
    236, 236, 236, 236, 366, 366, 260, 260, 260, 260, 393, 393, 399, 399, 394, 394, 395, 395, 396,
    396, 396, 396, 396, 396, 396, 396, 396, 396, 261, 262, 263, 263, 264, 397, 398, 398, 265, 266,
    266, 212, 198, 198, 198, 205, 206, 207, 267, 267, 267, 267, 267, 267, 267, 267, 267, 267, 267,
    267, 267, 267, 267, 267, 268, 268, 268, 268, 268, 268, 268, 268, 268, 377, 377, 377, 377, 377,
    377, 377, 377, 377, 377, 377, 377, 377, 377, 377, 269, 390, 336, 337, 338, 339, 340, 341, 342,
    343, 344, 345, 346, 347, 348, 349, 350, 378, 379, 380, 381, 382, 383, 384, 385, 386, 387, 388,
    389, 391, 392, 270, 270, 270, 271, 272, 273, 277, 277, 277, 277, 277, 277, 277, 277, 277, 277,
    277, 277, 277, 277, 277, 277, 277, 277, 277, 277, 277, 277, 278, 353, 353, 354, 354, 279, 280,
    309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 309, 357, 357, 358, 358,
    359, 359, 360, 360, 361, 361, 365, 365, 362, 362, 363, 363, 364, 364, 310, 310, 311, 312, 312,
    312, 313, 313, 313, 316, 316, 316, 314, 314, 314, 315, 315, 315, 321, 321, 321, 323, 323, 323,
    317, 317, 317, 318, 318, 318, 324, 324, 324, 322, 322, 322, 319, 319, 319, 320, 320, 320, 368,
    368, 368, 281, 282, 355, 355, 283, 290, 300, 356, 356, 287, 284, 285, 286, 288, 289, 291, 292,
    293, 294, 295, 296, 297, 298, 299, 428, 428, 426, 424, 425, 425, 427, 427, 427, 427, 427, 427,
    427, 427, 204, 204, 429, 429, 414, 415, 415, 422, 422, 416, 417, 417, 417, 417, 417, 419, 418,
    418, 420, 421, 421, 423, 423, 369, 369, 369, 369, 369, 369, 369, 370, 371, 372, 373, 374, 375,
    376, 274, 274, 275, 276, 226, 226, 237, 237, 238, 367, 367, 239, 240, 240, 213, 208, 208, 208,
    208, 208, 208, 301, 301, 301, 301, 301, 301, 301, 302, 303, 304, 305, 306, 307, 308, 325, 325,
    325, 325, 325, 325, 325, 325, 325, 325, 351, 351, 352, 352, 326, 327, 328, 329, 330, 331, 332,
    333, 334, 335};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2,  2,  3, 0,  4,  1,  1,  1, 1, 1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2,  2,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  3, 1, 2, 2, 2, 3, 0, 2, 2, 1, 1, 1, 3, 0, 2, 1, 1, 1,  2,
    3, 0, 2, 1, 1,  2,  2, 2,  2,  5,  5,  1, 1, 1, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 1,  0,  2,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 4, 5, 4,  4,  3, 3,  1,  1,  3,  0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  4, 4,  4,  4,  4,  4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,
    4, 4, 4, 4, 7,  4,  4, 4,  7,  4,  7,  8, 7, 7, 4, 7, 7, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,  4,
    4, 1, 1, 1, 4,  4,  6, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  6,
    0, 2, 0, 2, 11, 10, 1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 0, 2, 0, 2,  0,
    2, 0, 2, 0, 2,  0,  2, 0,  2,  14, 16, 9, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8,  4,
    4, 8, 4, 4, 8,  4,  4, 8,  4,  4,  8,  4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 0, 1, 2, 8, 8, 0, 2, 8,  8,
    8, 0, 2, 7, 4,  4,  4, 11, 11, 7,  4,  4, 7, 8, 8, 8, 4, 4, 1, 1, 4, 3, 0, 2, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 2,  2,  3, 0,  2,  2,  2,  1, 1, 1, 1, 1, 1, 4, 4, 7, 3, 1, 2, 2, 2, 1, 1, 1, 1,  1,
    1, 1, 6, 6, 4,  8,  8, 4,  8,  1,  1,  6, 6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1,  1,
    1, 1, 1, 1, 1,  1,  1, 4,  4,  4,  4,  4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4,
    4, 4, 4, 4, 4,  4,  4, 4};


#if YYDEBUG || 1
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const ParserGen::yytname_[] = {"\"EOF\"",
                                           "error",
                                           "\"invalid token\"",
                                           "ABS",
                                           "ACOS",
                                           "ACOSH",
                                           "ADD",
                                           "\"allElementsTrue\"",
                                           "AND",
                                           "\"anyElementTrue\"",
                                           "\"chars argument\"",
                                           "\"coll argument\"",
                                           "\"date argument\"",
                                           "\"dateString argument\"",
                                           "\"day argument\"",
                                           "\"filter\"",
                                           "\"find argument\"",
                                           "\"format argument\"",
                                           "\"hour argument\"",
                                           "\"input argument\"",
                                           "\"ISO 8601 argument\"",
                                           "\"ISO day of week argument\"",
                                           "\"ISO week argument\"",
                                           "\"ISO week year argument\"",
                                           "\"millisecond argument\"",
                                           "\"minute argument\"",
                                           "\"month argument\"",
                                           "\"onError argument\"",
                                           "\"onNull argument\"",
                                           "\"options argument\"",
                                           "\"pipeline argument\"",
                                           "\"regex argument\"",
                                           "\"replacement argument\"",
                                           "\"second argument\"",
                                           "\"size argument\"",
                                           "\"timezone argument\"",
                                           "\"to argument\"",
                                           "ASIN",
                                           "ASINH",
                                           "ATAN",
                                           "\"year argument\"",
                                           "ATAN2",
                                           "ATANH",
                                           "\"false\"",
                                           "\"true\"",
                                           "CEIL",
                                           "COMMENT",
                                           "CMP",
                                           "CONCAT",
                                           "CONST_EXPR",
                                           "CONVERT",
                                           "COS",
                                           "COSH",
                                           "DATE_FROM_PARTS",
                                           "DATE_FROM_STRING",
                                           "DATE_TO_PARTS",
                                           "DATE_TO_STRING",
                                           "DAY_OF_MONTH",
                                           "DAY_OF_WEEK",
                                           "DAY_OF_YEAR",
                                           "\"-1 (decimal)\"",
                                           "\"1 (decimal)\"",
                                           "\"zero (decimal)\"",
                                           "DEGREES_TO_RADIANS",
                                           "DIVIDE",
                                           "\"-1 (double)\"",
                                           "\"1 (double)\"",
                                           "\"zero (double)\"",
                                           "\"end of array\"",
                                           "\"end of object\"",
                                           "\"elemMatch operator\"",
                                           "EQ",
                                           "EXISTS",
                                           "EXPONENT",
                                           "FLOOR",
                                           "\"geoNearDistance\"",
                                           "\"geoNearPoint\"",
                                           "GT",
                                           "GTE",
                                           "HOUR",
                                           "ID",
                                           "INDEX_OF_BYTES",
                                           "INDEX_OF_CP",
                                           "\"indexKey\"",
                                           "\"-1 (int)\"",
                                           "\"1 (int)\"",
                                           "\"zero (int)\"",
                                           "ISO_DAY_OF_WEEK",
                                           "ISO_WEEK",
                                           "ISO_WEEK_YEAR",
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
                                           "MILLISECOND",
                                           "MINUTE",
                                           "MOD",
                                           "MONTH",
                                           "MULTIPLY",
                                           "NE",
                                           "NOR",
                                           "NOT",
                                           "OR",
                                           "POW",
                                           "RADIANS_TO_DEGREES",
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
                                           "SECOND",
                                           "\"setDifference\"",
                                           "\"setEquals\"",
                                           "\"setIntersection\"",
                                           "\"setIsSubset\"",
                                           "\"setUnion\"",
                                           "\"slice\"",
                                           "\"sortKey\"",
                                           "SIN",
                                           "SINH",
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
                                           "TAN",
                                           "TANH",
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
                                           "WEEK",
                                           "YEAR",
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
                                           "START_PROJECT",
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
                                           "sortFieldname",
                                           "idAsUserFieldname",
                                           "elemMatchAsUserFieldname",
                                           "idAsProjectionPath",
                                           "valueFieldname",
                                           "predFieldname",
                                           "aggregationProjectField",
                                           "aggregationProjectionObjectField",
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
                                           "typeArray",
                                           "typeValue",
                                           "pipeline",
                                           "stageList",
                                           "stage",
                                           "inhibitOptimization",
                                           "unionWith",
                                           "skip",
                                           "limit",
                                           "project",
                                           "sample",
                                           "aggregationProjectFields",
                                           "aggregationProjectionObjectFields",
                                           "topLevelAggregationProjection",
                                           "aggregationProjection",
                                           "projectionCommon",
                                           "aggregationProjectionObject",
                                           "num",
                                           "expression",
                                           "exprFixedTwoArg",
                                           "exprFixedThreeArg",
                                           "slice",
                                           "expressionArray",
                                           "expressionObject",
                                           "expressionFields",
                                           "maths",
                                           "meta",
                                           "add",
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
                                           "dateExps",
                                           "dateFromParts",
                                           "dateToParts",
                                           "dayOfMonth",
                                           "dayOfWeek",
                                           "dayOfYear",
                                           "hour",
                                           "isoDayOfWeek",
                                           "isoWeek",
                                           "isoWeekYear",
                                           "millisecond",
                                           "minute",
                                           "month",
                                           "second",
                                           "week",
                                           "year",
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
                                           "hourArg",
                                           "minuteArg",
                                           "secondArg",
                                           "millisecondArg",
                                           "dayArg",
                                           "isoWeekArg",
                                           "iso8601Arg",
                                           "monthArg",
                                           "isoDayOfWeekArg",
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
                                           "trig",
                                           "sin",
                                           "cos",
                                           "tan",
                                           "sinh",
                                           "cosh",
                                           "tanh",
                                           "asin",
                                           "acos",
                                           "atan",
                                           "asinh",
                                           "acosh",
                                           "atanh",
                                           "atan2",
                                           "degreesToRadians",
                                           "radiansToDegrees",
                                           "nonArrayExpression",
                                           "nonArrayCompoundExpression",
                                           "aggregationOperator",
                                           "aggregationOperatorWithoutSlice",
                                           "expressionSingletonArray",
                                           "singleArgExpression",
                                           "nonArrayNonObjExpression",
                                           "match",
                                           "predicates",
                                           "compoundMatchExprs",
                                           "predValue",
                                           "additionalExprs",
                                           "predicate",
                                           "logicalExpr",
                                           "operatorExpression",
                                           "notExpr",
                                           "existsExpr",
                                           "typeExpr",
                                           "commentExpr",
                                           "logicalExprField",
                                           "typeValues",
                                           "findProject",
                                           "findProjectFields",
                                           "topLevelFindProjection",
                                           "findProjection",
                                           "findProjectionSlice",
                                           "elemMatch",
                                           "findProjectionObject",
                                           "findProjectionObjectFields",
                                           "findProjectField",
                                           "findProjectionObjectField",
                                           "sortSpecs",
                                           "specList",
                                           "metaSort",
                                           "oneOrNegOne",
                                           "metaSortKeyword",
                                           "sortSpec",
                                           "start",
                                           "START_ORDERED_OBJECT",
                                           YY_NULLPTR};
#endif


#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    393,  393,  396,  399,  402,  409,  415,  416,  424,  427,  427,  427,  427,  427,  427,
    430,  440,  446,  456,  456,  456,  456,  460,  465,  470,  489,  492,  499,  502,  508,  522,
    523,  524,  528,  529,  530,  531,  532,  533,  534,  535,  536,  537,  538,  539,  542,  545,
    548,  551,  554,  557,  560,  563,  566,  569,  572,  575,  578,  581,  584,  587,  590,  593,
    594,  595,  596,  601,  610,  621,  622,  637,  644,  648,  656,  659,  665,  671,  674,  680,
    683,  684,  691,  692,  698,  701,  709,  709,  709,  713,  719,  725,  726,  733,  733,  737,
    746,  756,  762,  767,  777,  785,  786,  787,  790,  793,  800,  800,  800,  803,  811,  814,
    817,  820,  823,  826,  832,  838,  854,  857,  860,  863,  866,  869,  872,  875,  878,  881,
    884,  887,  890,  893,  896,  899,  902,  905,  908,  911,  914,  917,  920,  923,  926,  929,
    932,  940,  943,  946,  949,  952,  955,  958,  961,  964,  967,  970,  973,  976,  979,  982,
    985,  988,  991,  994,  997,  1000, 1003, 1006, 1009, 1012, 1015, 1018, 1021, 1024, 1027, 1030,
    1033, 1036, 1039, 1042, 1045, 1048, 1051, 1054, 1057, 1060, 1063, 1066, 1069, 1072, 1075, 1078,
    1081, 1084, 1087, 1090, 1093, 1096, 1099, 1102, 1105, 1108, 1111, 1114, 1117, 1120, 1123, 1126,
    1129, 1132, 1135, 1138, 1141, 1144, 1147, 1150, 1153, 1156, 1159, 1162, 1165, 1168, 1171, 1174,
    1177, 1180, 1183, 1186, 1189, 1192, 1195, 1198, 1201, 1204, 1207, 1210, 1213, 1216, 1219, 1222,
    1225, 1228, 1231, 1234, 1241, 1246, 1249, 1252, 1255, 1258, 1261, 1264, 1267, 1270, 1276, 1290,
    1304, 1310, 1316, 1322, 1328, 1334, 1340, 1346, 1352, 1358, 1364, 1370, 1376, 1382, 1385, 1388,
    1391, 1397, 1400, 1403, 1406, 1412, 1415, 1418, 1421, 1427, 1430, 1433, 1436, 1442, 1445, 1451,
    1452, 1453, 1454, 1455, 1456, 1457, 1458, 1459, 1460, 1461, 1462, 1463, 1464, 1465, 1466, 1467,
    1468, 1469, 1470, 1471, 1478, 1479, 1486, 1486, 1486, 1486, 1490, 1490, 1494, 1494, 1498, 1498,
    1502, 1502, 1506, 1506, 1506, 1506, 1506, 1506, 1506, 1507, 1507, 1507, 1512, 1519, 1525, 1529,
    1538, 1545, 1550, 1550, 1555, 1561, 1564, 1571, 1578, 1578, 1578, 1582, 1588, 1594, 1600, 1600,
    1600, 1600, 1600, 1600, 1600, 1600, 1600, 1600, 1600, 1600, 1600, 1601, 1601, 1601, 1605, 1608,
    1611, 1614, 1617, 1620, 1623, 1626, 1629, 1634, 1634, 1634, 1634, 1634, 1634, 1634, 1634, 1634,
    1634, 1634, 1634, 1634, 1635, 1635, 1639, 1646, 1652, 1657, 1662, 1668, 1673, 1678, 1683, 1689,
    1694, 1700, 1709, 1715, 1721, 1726, 1732, 1738, 1743, 1748, 1753, 1758, 1763, 1768, 1773, 1778,
    1783, 1788, 1793, 1798, 1803, 1809, 1809, 1809, 1813, 1820, 1827, 1834, 1834, 1834, 1834, 1834,
    1834, 1834, 1835, 1835, 1835, 1835, 1835, 1835, 1835, 1835, 1836, 1836, 1836, 1836, 1836, 1836,
    1836, 1840, 1850, 1853, 1859, 1862, 1869, 1878, 1887, 1887, 1887, 1887, 1887, 1887, 1887, 1887,
    1887, 1888, 1888, 1888, 1888, 1888, 1888, 1892, 1895, 1901, 1904, 1910, 1913, 1919, 1922, 1928,
    1931, 1937, 1940, 1946, 1949, 1955, 1958, 1964, 1967, 1973, 1979, 1988, 1996, 1999, 2003, 2009,
    2013, 2017, 2023, 2027, 2031, 2037, 2041, 2045, 2051, 2055, 2059, 2065, 2069, 2073, 2079, 2083,
    2087, 2093, 2097, 2101, 2107, 2111, 2115, 2121, 2125, 2129, 2135, 2139, 2143, 2149, 2153, 2157,
    2163, 2167, 2171, 2177, 2180, 2183, 2189, 2200, 2211, 2214, 2220, 2228, 2236, 2244, 2247, 2252,
    2261, 2267, 2273, 2279, 2289, 2299, 2306, 2313, 2320, 2328, 2336, 2344, 2352, 2358, 2364, 2367,
    2373, 2379, 2384, 2387, 2394, 2397, 2400, 2403, 2406, 2409, 2412, 2415, 2420, 2422, 2432, 2434,
    2440, 2459, 2462, 2469, 2472, 2478, 2492, 2493, 2494, 2495, 2496, 2500, 2506, 2509, 2517, 2524,
    2528, 2536, 2539, 2545, 2545, 2545, 2545, 2545, 2545, 2546, 2550, 2556, 2562, 2569, 2580, 2591,
    2598, 2609, 2609, 2613, 2620, 2627, 2627, 2631, 2631, 2635, 2641, 2642, 2649, 2655, 2658, 2665,
    2672, 2673, 2674, 2675, 2676, 2677, 2680, 2680, 2680, 2680, 2680, 2680, 2680, 2682, 2687, 2692,
    2697, 2702, 2707, 2712, 2718, 2719, 2720, 2721, 2722, 2723, 2724, 2725, 2726, 2727, 2732, 2735,
    2742, 2745, 2751, 2761, 2766, 2771, 2776, 2781, 2786, 2791, 2796, 2801};

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
#line 9422 "parser_gen.cpp"

#line 2805 "grammar.yy"
