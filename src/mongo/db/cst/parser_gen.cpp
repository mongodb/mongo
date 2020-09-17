// A Bison parser, made by GNU Bison 3.5.

// Skeleton implementation for Bison LALR(1) parsers in C++

// Copyright (C) 2002-2015, 2018-2019 Free Software Foundation, Inc.

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


#include "parser_gen.hpp"


// Unqualified %code blocks.
#line 82 "src/mongo/db/cst/grammar.yy"

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

#line 72 "src/mongo/db/cst/parser_gen.cpp"


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

#line 57 "src/mongo/db/cst/grammar.yy"
namespace mongo {
#line 164 "src/mongo/db/cst/parser_gen.cpp"


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
| Symbol types.  |
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

ParserGen::symbol_number_type ParserGen::by_state::type_get() const YY_NOEXCEPT {
    if (state == empty_state)
        return empty_symbol;
    else
        return yystos_[state];
}

ParserGen::stack_symbol_type::stack_symbol_type() {}

ParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
    : super_type(YY_MOVE(that.state), YY_MOVE(that.location)) {
    switch (that.type_get()) {
        case 182:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 189:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 191:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 188:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 187:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 190:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 185:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 195:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 184:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 196:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 198:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 197:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 186:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 183:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 194:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 192:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 193:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 177:  // "fieldname containing dotted path"
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
    switch (that.type_get()) {
        case 182:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 189:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 191:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 188:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 187:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 190:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 185:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 195:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 184:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 196:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 198:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 197:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 186:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 183:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 194:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 192:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 193:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 177:  // "fieldname containing dotted path"
            value.move<std::vector<std::string>>(YY_MOVE(that.value));
            break;

        default:
            break;
    }

    // that is emptied.
    that.type = empty_symbol;
}

#if YY_CPLUSPLUS < 201103L
ParserGen::stack_symbol_type& ParserGen::stack_symbol_type::operator=(
    const stack_symbol_type& that) {
    state = that.state;
    switch (that.type_get()) {
        case 182:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 189:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 191:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 188:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 187:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 190:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 185:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 195:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 184:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 196:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 198:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 197:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 186:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 183:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 194:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 192:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 193:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.copy<std::string>(that.value);
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.copy<std::vector<CNode>>(that.value);
            break;

        case 177:  // "fieldname containing dotted path"
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
    switch (that.type_get()) {
        case 182:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 189:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 191:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 188:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 187:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 190:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 223:  // dbPointer
        case 224:  // javascript
        case 225:  // symbol
        case 226:  // javascriptWScope
        case 227:  // int
        case 228:  // timestamp
        case 229:  // long
        case 230:  // double
        case 231:  // decimal
        case 232:  // minKey
        case 233:  // maxKey
        case 234:  // value
        case 235:  // string
        case 236:  // aggregationFieldPath
        case 237:  // binary
        case 238:  // undefined
        case 239:  // objectId
        case 240:  // bool
        case 241:  // date
        case 242:  // null
        case 243:  // regex
        case 244:  // simpleValue
        case 245:  // compoundValue
        case 246:  // valueArray
        case 247:  // valueObject
        case 248:  // valueFields
        case 249:  // variable
        case 250:  // typeArray
        case 251:  // typeValue
        case 252:  // pipeline
        case 253:  // stageList
        case 254:  // stage
        case 255:  // inhibitOptimization
        case 256:  // unionWith
        case 257:  // skip
        case 258:  // limit
        case 259:  // matchStage
        case 260:  // project
        case 261:  // sample
        case 262:  // aggregationProjectFields
        case 263:  // aggregationProjectionObjectFields
        case 264:  // topLevelAggregationProjection
        case 265:  // aggregationProjection
        case 266:  // projectionCommon
        case 267:  // aggregationProjectionObject
        case 268:  // num
        case 269:  // expression
        case 270:  // exprFixedTwoArg
        case 271:  // exprFixedThreeArg
        case 272:  // slice
        case 273:  // expressionArray
        case 274:  // expressionObject
        case 275:  // expressionFields
        case 276:  // maths
        case 277:  // meta
        case 278:  // add
        case 279:  // boolExprs
        case 280:  // and
        case 281:  // or
        case 282:  // not
        case 283:  // literalEscapes
        case 284:  // const
        case 285:  // literal
        case 286:  // stringExps
        case 287:  // concat
        case 288:  // dateFromString
        case 289:  // dateToString
        case 290:  // indexOfBytes
        case 291:  // indexOfCP
        case 292:  // ltrim
        case 293:  // regexFind
        case 294:  // regexFindAll
        case 295:  // regexMatch
        case 296:  // regexArgs
        case 297:  // replaceOne
        case 298:  // replaceAll
        case 299:  // rtrim
        case 300:  // split
        case 301:  // strLenBytes
        case 302:  // strLenCP
        case 303:  // strcasecmp
        case 304:  // substr
        case 305:  // substrBytes
        case 306:  // substrCP
        case 307:  // toLower
        case 308:  // toUpper
        case 309:  // trim
        case 310:  // compExprs
        case 311:  // cmp
        case 312:  // eq
        case 313:  // gt
        case 314:  // gte
        case 315:  // lt
        case 316:  // lte
        case 317:  // ne
        case 318:  // dateExps
        case 319:  // dateFromParts
        case 320:  // dateToParts
        case 321:  // dayOfMonth
        case 322:  // dayOfWeek
        case 323:  // dayOfYear
        case 324:  // hour
        case 325:  // isoDayOfWeek
        case 326:  // isoWeek
        case 327:  // isoWeekYear
        case 328:  // millisecond
        case 329:  // minute
        case 330:  // month
        case 331:  // second
        case 332:  // week
        case 333:  // year
        case 334:  // typeExpression
        case 335:  // convert
        case 336:  // toBool
        case 337:  // toDate
        case 338:  // toDecimal
        case 339:  // toDouble
        case 340:  // toInt
        case 341:  // toLong
        case 342:  // toObjectId
        case 343:  // toString
        case 344:  // type
        case 345:  // abs
        case 346:  // ceil
        case 347:  // divide
        case 348:  // exponent
        case 349:  // floor
        case 350:  // ln
        case 351:  // log
        case 352:  // logten
        case 353:  // mod
        case 354:  // multiply
        case 355:  // pow
        case 356:  // round
        case 357:  // sqrt
        case 358:  // subtract
        case 359:  // trunc
        case 378:  // setExpression
        case 379:  // allElementsTrue
        case 380:  // anyElementTrue
        case 381:  // setDifference
        case 382:  // setEquals
        case 383:  // setIntersection
        case 384:  // setIsSubset
        case 385:  // setUnion
        case 386:  // trig
        case 387:  // sin
        case 388:  // cos
        case 389:  // tan
        case 390:  // sinh
        case 391:  // cosh
        case 392:  // tanh
        case 393:  // asin
        case 394:  // acos
        case 395:  // atan
        case 396:  // asinh
        case 397:  // acosh
        case 398:  // atanh
        case 399:  // atan2
        case 400:  // degreesToRadians
        case 401:  // radiansToDegrees
        case 402:  // nonArrayExpression
        case 403:  // nonArrayCompoundExpression
        case 404:  // aggregationOperator
        case 405:  // aggregationOperatorWithoutSlice
        case 406:  // expressionSingletonArray
        case 407:  // singleArgExpression
        case 408:  // nonArrayNonObjExpression
        case 409:  // matchExpression
        case 410:  // predicates
        case 411:  // compoundMatchExprs
        case 412:  // predValue
        case 413:  // additionalExprs
        case 428:  // textArgCaseSensitive
        case 429:  // textArgDiacriticSensitive
        case 430:  // textArgLanguage
        case 431:  // textArgSearch
        case 432:  // findProject
        case 433:  // findProjectFields
        case 434:  // topLevelFindProjection
        case 435:  // findProjection
        case 436:  // findProjectionSlice
        case 437:  // elemMatch
        case 438:  // findProjectionObject
        case 439:  // findProjectionObjectFields
        case 442:  // sortSpecs
        case 443:  // specList
        case 444:  // metaSort
        case 445:  // oneOrNegOne
        case 446:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 204:  // aggregationProjectionFieldname
        case 205:  // projectionFieldname
        case 206:  // expressionFieldname
        case 207:  // stageAsUserFieldname
        case 208:  // argAsUserFieldname
        case 209:  // argAsProjectionPath
        case 210:  // aggExprAsUserFieldname
        case 211:  // invariableUserFieldname
        case 212:  // sortFieldname
        case 213:  // idAsUserFieldname
        case 214:  // elemMatchAsUserFieldname
        case 215:  // idAsProjectionPath
        case 216:  // valueFieldname
        case 217:  // predFieldname
        case 423:  // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case 185:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 195:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 184:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 196:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 198:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 197:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 186:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 183:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 194:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 192:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 193:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 218:  // aggregationProjectField
        case 219:  // aggregationProjectionObjectField
        case 220:  // expressionField
        case 221:  // valueField
        case 360:  // onErrorArg
        case 361:  // onNullArg
        case 362:  // formatArg
        case 363:  // timezoneArg
        case 364:  // charsArg
        case 365:  // optionsArg
        case 366:  // hourArg
        case 367:  // minuteArg
        case 368:  // secondArg
        case 369:  // millisecondArg
        case 370:  // dayArg
        case 371:  // isoWeekArg
        case 372:  // iso8601Arg
        case 373:  // monthArg
        case 374:  // isoDayOfWeekArg
        case 414:  // predicate
        case 415:  // fieldPredicate
        case 416:  // logicalExpr
        case 417:  // operatorExpression
        case 418:  // notExpr
        case 419:  // matchMod
        case 420:  // existsExpr
        case 421:  // typeExpr
        case 422:  // commentExpr
        case 425:  // matchExpr
        case 426:  // matchText
        case 427:  // matchWhere
        case 440:  // findProjectField
        case 441:  // findProjectionObjectField
        case 447:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 176:  // "fieldname"
        case 178:  // "$-prefixed fieldname"
        case 179:  // "string"
        case 180:  // "$-prefixed string"
        case 181:  // "$$-prefixed string"
        case 222:  // arg
            value.move<std::string>(that.value);
            break;

        case 375:  // expressions
        case 376:  // values
        case 377:  // exprZeroToTwo
        case 424:  // typeValues
            value.move<std::vector<CNode>>(that.value);
            break;

        case 177:  // "fieldname containing dotted path"
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
    int yyr = yypgoto_[yysym - yyntokens_] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - yyntokens_];
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

        // Accept?
        if (yystack_[0].state == yyfinal_)
            YYACCEPT;

        goto yybackup;


    /*-----------.
    | yybackup.  |
    `-----------*/
    yybackup:
        // Try to take a decision without lookahead.
        yyn = yypact_[yystack_[0].state];
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
        yypush_("Shifting", static_cast<state_type>(yyn), YY_MOVE(yyla));
        goto yynewstate;


    /*-----------------------------------------------------------.
    | yydefault -- do the default action for the current state.  |
    `-----------------------------------------------------------*/
    yydefault:
        yyn = yydefact_[yystack_[0].state];
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
                case 182:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 189:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 191:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 188:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 187:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 190:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 223:  // dbPointer
                case 224:  // javascript
                case 225:  // symbol
                case 226:  // javascriptWScope
                case 227:  // int
                case 228:  // timestamp
                case 229:  // long
                case 230:  // double
                case 231:  // decimal
                case 232:  // minKey
                case 233:  // maxKey
                case 234:  // value
                case 235:  // string
                case 236:  // aggregationFieldPath
                case 237:  // binary
                case 238:  // undefined
                case 239:  // objectId
                case 240:  // bool
                case 241:  // date
                case 242:  // null
                case 243:  // regex
                case 244:  // simpleValue
                case 245:  // compoundValue
                case 246:  // valueArray
                case 247:  // valueObject
                case 248:  // valueFields
                case 249:  // variable
                case 250:  // typeArray
                case 251:  // typeValue
                case 252:  // pipeline
                case 253:  // stageList
                case 254:  // stage
                case 255:  // inhibitOptimization
                case 256:  // unionWith
                case 257:  // skip
                case 258:  // limit
                case 259:  // matchStage
                case 260:  // project
                case 261:  // sample
                case 262:  // aggregationProjectFields
                case 263:  // aggregationProjectionObjectFields
                case 264:  // topLevelAggregationProjection
                case 265:  // aggregationProjection
                case 266:  // projectionCommon
                case 267:  // aggregationProjectionObject
                case 268:  // num
                case 269:  // expression
                case 270:  // exprFixedTwoArg
                case 271:  // exprFixedThreeArg
                case 272:  // slice
                case 273:  // expressionArray
                case 274:  // expressionObject
                case 275:  // expressionFields
                case 276:  // maths
                case 277:  // meta
                case 278:  // add
                case 279:  // boolExprs
                case 280:  // and
                case 281:  // or
                case 282:  // not
                case 283:  // literalEscapes
                case 284:  // const
                case 285:  // literal
                case 286:  // stringExps
                case 287:  // concat
                case 288:  // dateFromString
                case 289:  // dateToString
                case 290:  // indexOfBytes
                case 291:  // indexOfCP
                case 292:  // ltrim
                case 293:  // regexFind
                case 294:  // regexFindAll
                case 295:  // regexMatch
                case 296:  // regexArgs
                case 297:  // replaceOne
                case 298:  // replaceAll
                case 299:  // rtrim
                case 300:  // split
                case 301:  // strLenBytes
                case 302:  // strLenCP
                case 303:  // strcasecmp
                case 304:  // substr
                case 305:  // substrBytes
                case 306:  // substrCP
                case 307:  // toLower
                case 308:  // toUpper
                case 309:  // trim
                case 310:  // compExprs
                case 311:  // cmp
                case 312:  // eq
                case 313:  // gt
                case 314:  // gte
                case 315:  // lt
                case 316:  // lte
                case 317:  // ne
                case 318:  // dateExps
                case 319:  // dateFromParts
                case 320:  // dateToParts
                case 321:  // dayOfMonth
                case 322:  // dayOfWeek
                case 323:  // dayOfYear
                case 324:  // hour
                case 325:  // isoDayOfWeek
                case 326:  // isoWeek
                case 327:  // isoWeekYear
                case 328:  // millisecond
                case 329:  // minute
                case 330:  // month
                case 331:  // second
                case 332:  // week
                case 333:  // year
                case 334:  // typeExpression
                case 335:  // convert
                case 336:  // toBool
                case 337:  // toDate
                case 338:  // toDecimal
                case 339:  // toDouble
                case 340:  // toInt
                case 341:  // toLong
                case 342:  // toObjectId
                case 343:  // toString
                case 344:  // type
                case 345:  // abs
                case 346:  // ceil
                case 347:  // divide
                case 348:  // exponent
                case 349:  // floor
                case 350:  // ln
                case 351:  // log
                case 352:  // logten
                case 353:  // mod
                case 354:  // multiply
                case 355:  // pow
                case 356:  // round
                case 357:  // sqrt
                case 358:  // subtract
                case 359:  // trunc
                case 378:  // setExpression
                case 379:  // allElementsTrue
                case 380:  // anyElementTrue
                case 381:  // setDifference
                case 382:  // setEquals
                case 383:  // setIntersection
                case 384:  // setIsSubset
                case 385:  // setUnion
                case 386:  // trig
                case 387:  // sin
                case 388:  // cos
                case 389:  // tan
                case 390:  // sinh
                case 391:  // cosh
                case 392:  // tanh
                case 393:  // asin
                case 394:  // acos
                case 395:  // atan
                case 396:  // asinh
                case 397:  // acosh
                case 398:  // atanh
                case 399:  // atan2
                case 400:  // degreesToRadians
                case 401:  // radiansToDegrees
                case 402:  // nonArrayExpression
                case 403:  // nonArrayCompoundExpression
                case 404:  // aggregationOperator
                case 405:  // aggregationOperatorWithoutSlice
                case 406:  // expressionSingletonArray
                case 407:  // singleArgExpression
                case 408:  // nonArrayNonObjExpression
                case 409:  // matchExpression
                case 410:  // predicates
                case 411:  // compoundMatchExprs
                case 412:  // predValue
                case 413:  // additionalExprs
                case 428:  // textArgCaseSensitive
                case 429:  // textArgDiacriticSensitive
                case 430:  // textArgLanguage
                case 431:  // textArgSearch
                case 432:  // findProject
                case 433:  // findProjectFields
                case 434:  // topLevelFindProjection
                case 435:  // findProjection
                case 436:  // findProjectionSlice
                case 437:  // elemMatch
                case 438:  // findProjectionObject
                case 439:  // findProjectionObjectFields
                case 442:  // sortSpecs
                case 443:  // specList
                case 444:  // metaSort
                case 445:  // oneOrNegOne
                case 446:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 204:  // aggregationProjectionFieldname
                case 205:  // projectionFieldname
                case 206:  // expressionFieldname
                case 207:  // stageAsUserFieldname
                case 208:  // argAsUserFieldname
                case 209:  // argAsProjectionPath
                case 210:  // aggExprAsUserFieldname
                case 211:  // invariableUserFieldname
                case 212:  // sortFieldname
                case 213:  // idAsUserFieldname
                case 214:  // elemMatchAsUserFieldname
                case 215:  // idAsProjectionPath
                case 216:  // valueFieldname
                case 217:  // predFieldname
                case 423:  // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 185:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 195:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 184:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 196:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 198:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 197:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 186:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 183:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 194:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 192:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 193:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 218:  // aggregationProjectField
                case 219:  // aggregationProjectionObjectField
                case 220:  // expressionField
                case 221:  // valueField
                case 360:  // onErrorArg
                case 361:  // onNullArg
                case 362:  // formatArg
                case 363:  // timezoneArg
                case 364:  // charsArg
                case 365:  // optionsArg
                case 366:  // hourArg
                case 367:  // minuteArg
                case 368:  // secondArg
                case 369:  // millisecondArg
                case 370:  // dayArg
                case 371:  // isoWeekArg
                case 372:  // iso8601Arg
                case 373:  // monthArg
                case 374:  // isoDayOfWeekArg
                case 414:  // predicate
                case 415:  // fieldPredicate
                case 416:  // logicalExpr
                case 417:  // operatorExpression
                case 418:  // notExpr
                case 419:  // matchMod
                case 420:  // existsExpr
                case 421:  // typeExpr
                case 422:  // commentExpr
                case 425:  // matchExpr
                case 426:  // matchText
                case 427:  // matchWhere
                case 440:  // findProjectField
                case 441:  // findProjectionObjectField
                case 447:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 176:  // "fieldname"
                case 178:  // "$-prefixed fieldname"
                case 179:  // "string"
                case 180:  // "$-prefixed string"
                case 181:  // "$$-prefixed string"
                case 222:  // arg
                    yylhs.value.emplace<std::string>();
                    break;

                case 375:  // expressions
                case 376:  // values
                case 377:  // exprZeroToTwo
                case 424:  // typeValues
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                case 177:  // "fieldname containing dotted path"
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
#line 405 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 3:
#line 408 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 4:
#line 411 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 5:
#line 414 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2298 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 6:
#line 421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 7:
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 2312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 8:
#line 428 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2320 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 9:
#line 436 "src/mongo/db/cst/grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2326 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 10:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2332 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 11:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2338 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 12:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2344 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 13:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2350 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 14:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2356 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 15:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2362 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 16:
#line 439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2368 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 17:
#line 442 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2380 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 18:
#line 452 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2388 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 19:
#line 458 "src/mongo/db/cst/grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2401 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 20:
#line 468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2407 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 21:
#line 468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2413 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 22:
#line 468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2419 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 23:
#line 468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2425 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 24:
#line 472 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2433 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 25:
#line 477 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2441 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 26:
#line 482 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2449 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 27:
#line 488 "src/mongo/db/cst/grammar.yy"
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
#line 2470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 28:
#line 507 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 29:
#line 510 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2487 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 30:
#line 517 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2495 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 31:
#line 520 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2503 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 32:
#line 526 "src/mongo/db/cst/grammar.yy"
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
#line 2519 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 33:
#line 540 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2525 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 34:
#line 541 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2531 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 35:
#line 542 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2537 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 36:
#line 546 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2543 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 37:
#line 547 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2549 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 38:
#line 548 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2555 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 39:
#line 549 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2561 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 40:
#line 550 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2567 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 41:
#line 551 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2573 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 42:
#line 552 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2579 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 43:
#line 553 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2585 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 44:
#line 554 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2591 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 45:
#line 555 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2597 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 46:
#line 556 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2603 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 47:
#line 557 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2611 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 48:
#line 560 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2619 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 49:
#line 563 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2627 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 50:
#line 566 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2635 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 51:
#line 569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2643 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 52:
#line 572 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2651 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 53:
#line 575 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2659 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 54:
#line 578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2667 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 55:
#line 581 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2675 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 56:
#line 584 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2683 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 57:
#line 587 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2691 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 58:
#line 590 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2699 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 59:
#line 593 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2707 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 60:
#line 596 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2715 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 61:
#line 599 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2723 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 62:
#line 602 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2731 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 63:
#line 605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2739 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 64:
#line 608 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2747 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 65:
#line 611 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2753 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 66:
#line 612 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2759 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 67:
#line 613 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2765 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 68:
#line 614 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2771 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 69:
#line 619 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2781 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 70:
#line 628 "src/mongo/db/cst/grammar.yy"
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
#line 2797 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 71:
#line 639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2803 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 72:
#line 640 "src/mongo/db/cst/grammar.yy"
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
#line 2819 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 73:
#line 655 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2827 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 74:
#line 662 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2836 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 75:
#line 666 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2845 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 76:
#line 674 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2853 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 77:
#line 677 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2861 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 78:
#line 683 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2869 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 79:
#line 689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2877 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 80:
#line 692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2886 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 81:
#line 699 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2892 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 82:
#line 700 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2898 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 83:
#line 703 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2904 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 84:
#line 704 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2910 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 85:
#line 705 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2916 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 86:
#line 706 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2922 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 87:
#line 709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2930 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 88:
#line 718 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2936 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 89:
#line 719 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2944 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 90:
#line 725 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2952 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 91:
#line 728 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2961 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 92:
#line 736 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2967 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 93:
#line 736 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2973 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 94:
#line 736 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2979 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 95:
#line 736 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2985 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 96:
#line 740 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2993 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 97:
#line 746 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3001 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 98:
#line 752 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 3007 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 99:
#line 753 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3016 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 100:
#line 760 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3022 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 101:
#line 760 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3028 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 102:
#line 764 "src/mongo/db/cst/grammar.yy"
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
#line 3042 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 103:
#line 773 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 3054 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 104:
#line 783 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3062 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 105:
#line 789 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3070 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 106:
#line 794 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 3081 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 107:
#line 803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::matchMod,
                            CNode{CNode::ArrayChildren{
                                YY_MOVE(yystack_[2].value.as<CNode>()),
                                YY_MOVE(yystack_[1].value.as<CNode>()),
                            }}};
                    }
#line 3092 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 108:
#line 813 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 3102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 109:
#line 821 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 3108 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 110:
#line 822 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 3114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 111:
#line 823 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 3120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 112:
#line 826 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 3128 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 113:
#line 829 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3137 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 114:
#line 836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3143 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 115:
#line 836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3149 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 116:
#line 836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3155 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 117:
#line 839 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3163 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 118:
#line 845 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::expr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3171 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 119:
#line 857 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::text,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::caseSensitive,
                                 YY_MOVE(yystack_[4].value.as<CNode>())},
                                {KeyFieldname::diacriticSensitive,
                                 YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::language, YY_MOVE(yystack_[2].value.as<CNode>())},
                                {KeyFieldname::search, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}};
                    }
#line 3187 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 120:
#line 870 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3195 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 121:
#line 873 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3203 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 122:
#line 878 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3211 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 123:
#line 881 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3219 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 124:
#line 886 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3227 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 125:
#line 889 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3235 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 126:
#line 894 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3243 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 127:
#line 900 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::where, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3249 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 128:
#line 901 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::where, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3255 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 129:
#line 907 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 3263 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 130:
#line 910 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 3271 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 131:
#line 913 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 3279 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 132:
#line 916 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 3287 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 133:
#line 919 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$match"};
                    }
#line 3295 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 134:
#line 922 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 3303 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 135:
#line 925 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 3311 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 136:
#line 931 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3319 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 137:
#line 937 "src/mongo/db/cst/grammar.yy"
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
#line 3334 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 138:
#line 953 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 3342 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 139:
#line 956 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 3350 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 140:
#line 959 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 3358 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 141:
#line 962 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 3366 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 142:
#line 965 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 3374 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 143:
#line 968 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 3382 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 144:
#line 971 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 3390 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 145:
#line 974 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 3398 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 146:
#line 977 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 3406 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 147:
#line 980 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 3414 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 148:
#line 983 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 3422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 149:
#line 986 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 3430 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 150:
#line 989 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3438 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 151:
#line 992 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 152:
#line 995 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3454 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 153:
#line 998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3462 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 154:
#line 1001 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"hour"};
                    }
#line 3470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 155:
#line 1004 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"year"};
                    }
#line 3478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 156:
#line 1007 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"minute"};
                    }
#line 3486 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 157:
#line 1010 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"second"};
                    }
#line 3494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 158:
#line 1013 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"millisecond"};
                    }
#line 3502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 159:
#line 1016 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"day"};
                    }
#line 3510 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 160:
#line 1019 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoDayOfWeek"};
                    }
#line 3518 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 161:
#line 1022 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeek"};
                    }
#line 3526 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 162:
#line 1025 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeekYear"};
                    }
#line 3534 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 163:
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"iso8601"};
                    }
#line 3542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 164:
#line 1031 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"month"};
                    }
#line 3550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 165:
#line 1034 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$search"};
                    }
#line 3558 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 166:
#line 1037 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$language"};
                    }
#line 3566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 167:
#line 1040 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$caseSensitive"};
                    }
#line 3574 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 168:
#line 1043 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$diacriticSensitive"};
                    }
#line 3582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 169:
#line 1051 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 170:
#line 1054 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 171:
#line 1057 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 172:
#line 1060 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 173:
#line 1063 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3622 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 174:
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3630 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 175:
#line 1069 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3638 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 176:
#line 1072 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3646 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 177:
#line 1075 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 178:
#line 1078 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3662 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 179:
#line 1081 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3670 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 180:
#line 1084 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3678 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 181:
#line 1087 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3686 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 182:
#line 1090 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3694 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 183:
#line 1093 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3702 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 184:
#line 1096 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3710 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 185:
#line 1099 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3718 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 186:
#line 1102 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 187:
#line 1105 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3734 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 188:
#line 1108 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3742 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 189:
#line 1111 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 190:
#line 1114 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 191:
#line 1117 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3766 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 192:
#line 1120 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 193:
#line 1123 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3782 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 194:
#line 1126 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3790 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 195:
#line 1129 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3798 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 196:
#line 1132 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 197:
#line 1135 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3814 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 198:
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 199:
#line 1141 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3830 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 200:
#line 1144 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3838 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 201:
#line 1147 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 202:
#line 1150 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3854 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 203:
#line 1153 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3862 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 204:
#line 1156 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3870 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 205:
#line 1159 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3878 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 206:
#line 1162 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3886 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 207:
#line 1165 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3894 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 208:
#line 1168 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3902 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 209:
#line 1171 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3910 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 210:
#line 1174 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromParts"};
                    }
#line 3918 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 211:
#line 1177 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToParts"};
                    }
#line 3926 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 212:
#line 1180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfMonth"};
                    }
#line 3934 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 213:
#line 1183 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfWeek"};
                    }
#line 3942 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 214:
#line 1186 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfYear"};
                    }
#line 3950 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 215:
#line 1189 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$hour"};
                    }
#line 3958 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 216:
#line 1192 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoDayOfWeek"};
                    }
#line 3966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 217:
#line 1195 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeek"};
                    }
#line 3974 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 218:
#line 1198 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeekYear"};
                    }
#line 3982 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 219:
#line 1201 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$millisecond"};
                    }
#line 3990 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 220:
#line 1204 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$minute"};
                    }
#line 3998 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 221:
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$month"};
                    }
#line 4006 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 222:
#line 1210 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$second"};
                    }
#line 4014 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 223:
#line 1213 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$week"};
                    }
#line 4022 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 224:
#line 1216 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$year"};
                    }
#line 4030 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 225:
#line 1219 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 4038 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 226:
#line 1222 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 4046 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 227:
#line 1225 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 4054 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 228:
#line 1228 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 4062 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 229:
#line 1231 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 4070 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 230:
#line 1234 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 4078 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 231:
#line 1237 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 4086 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 232:
#line 1240 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 4094 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 233:
#line 1243 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 4102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 234:
#line 1246 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 4110 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 235:
#line 1249 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 4118 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 236:
#line 1252 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 4126 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 237:
#line 1255 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 4134 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 238:
#line 1258 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 4142 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 239:
#line 1261 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 4150 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 240:
#line 1264 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 4158 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 241:
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 4166 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 242:
#line 1270 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 4174 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 243:
#line 1273 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 4182 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 244:
#line 1276 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 4190 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 245:
#line 1279 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 4198 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 246:
#line 1282 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 4206 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 247:
#line 1285 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 4214 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 248:
#line 1288 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 4222 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 249:
#line 1291 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 4230 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 250:
#line 1294 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 4238 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 251:
#line 1297 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 4246 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 252:
#line 1300 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 4254 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 253:
#line 1303 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 4262 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 254:
#line 1306 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 4270 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 255:
#line 1309 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 4278 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 256:
#line 1312 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 4286 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 257:
#line 1315 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 4294 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 258:
#line 1318 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 4302 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 259:
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 4310 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 260:
#line 1324 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 4318 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 261:
#line 1327 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 4326 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 262:
#line 1330 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 4334 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 263:
#line 1333 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 4342 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 264:
#line 1336 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 4350 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 265:
#line 1339 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 4358 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 266:
#line 1342 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 4366 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 267:
#line 1345 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 4374 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 268:
#line 1352 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 4382 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 269:
#line 1357 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 4390 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 270:
#line 1360 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 4398 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 271:
#line 1363 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 4406 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 272:
#line 1366 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 4414 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 273:
#line 1369 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 4422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 274:
#line 1372 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 4430 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 275:
#line 1375 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 4438 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 276:
#line 1378 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 4446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 277:
#line 1381 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 4454 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 278:
#line 1387 "src/mongo/db/cst/grammar.yy"
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
#line 4470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 279:
#line 1401 "src/mongo/db/cst/grammar.yy"
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
#line 4486 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 280:
#line 1415 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 4494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 281:
#line 1421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 4502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 282:
#line 1427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 4510 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 283:
#line 1433 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 4518 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 284:
#line 1439 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 4526 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 285:
#line 1445 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 4534 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 286:
#line 1451 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 4542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 287:
#line 1457 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 4550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 288:
#line 1463 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 4558 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 289:
#line 1469 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 4566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 290:
#line 1475 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 4574 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 291:
#line 1481 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 4582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 292:
#line 1487 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 4590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 293:
#line 1493 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 4598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 294:
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 4606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 295:
#line 1499 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 4614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 296:
#line 1502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 4622 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 297:
#line 1508 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 4630 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 298:
#line 1511 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 4638 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 299:
#line 1514 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 4646 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 300:
#line 1517 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 4654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 301:
#line 1523 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 4662 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 302:
#line 1526 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 4670 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 303:
#line 1529 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4678 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 304:
#line 1532 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4686 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 305:
#line 1538 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4694 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 306:
#line 1541 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4702 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 307:
#line 1544 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4710 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 308:
#line 1547 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4718 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 309:
#line 1553 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 310:
#line 1556 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4734 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 311:
#line 1562 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4740 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 312:
#line 1563 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4746 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 313:
#line 1564 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4752 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 314:
#line 1565 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 315:
#line 1566 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4764 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 316:
#line 1567 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4770 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 317:
#line 1568 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4776 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 318:
#line 1569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4782 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 319:
#line 1570 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4788 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 320:
#line 1571 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4794 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 321:
#line 1572 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4800 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 322:
#line 1573 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 323:
#line 1574 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4812 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 324:
#line 1575 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4818 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 325:
#line 1576 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4824 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 326:
#line 1577 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4830 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 327:
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4836 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 328:
#line 1579 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 329:
#line 1580 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4848 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 330:
#line 1581 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4854 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 331:
#line 1582 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4860 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 332:
#line 1589 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 4866 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 333:
#line 1590 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4875 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 334:
#line 1597 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4881 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 335:
#line 1597 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4887 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 336:
#line 1597 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4893 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 337:
#line 1597 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4899 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 338:
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4905 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 339:
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4911 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 340:
#line 1605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4917 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 341:
#line 1605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4923 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 342:
#line 1609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4929 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 343:
#line 1609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4935 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 344:
#line 1613 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4941 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 345:
#line 1613 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4947 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 346:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4953 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 347:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4959 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 348:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4965 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 349:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4971 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 350:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4977 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 351:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4983 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 352:
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4989 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 353:
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4995 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 354:
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5001 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 355:
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5007 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 356:
#line 1623 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5015 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 357:
#line 1630 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5023 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 358:
#line 1636 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5032 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 359:
#line 1640 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5041 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 360:
#line 1649 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5049 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 361:
#line 1656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5057 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 362:
#line 1661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5063 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 363:
#line 1661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5069 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 364:
#line 1666 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5077 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 365:
#line 1672 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5085 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 366:
#line 1675 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5094 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 367:
#line 1682 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 368:
#line 1689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5108 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 369:
#line 1689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 370:
#line 1689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 371:
#line 1693 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 5128 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 372:
#line 1699 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$elemMatch"};
                    }
#line 5136 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 373:
#line 1705 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 5144 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 374:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5150 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 375:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5156 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 376:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 377:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 378:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5174 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 379:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5180 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 380:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 381:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5192 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 382:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5198 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 383:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5204 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 384:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 385:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5216 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 386:
#line 1711 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5222 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 387:
#line 1712 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5228 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 388:
#line 1712 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 389:
#line 1712 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5240 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 390:
#line 1716 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 5248 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 391:
#line 1719 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 5256 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 392:
#line 1722 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 5264 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 393:
#line 1725 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 5272 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 394:
#line 1728 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 5280 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 395:
#line 1731 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 5288 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 396:
#line 1734 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 5296 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 397:
#line 1737 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 5304 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 398:
#line 1740 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 5312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 399:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5318 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 400:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5324 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 401:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 402:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5336 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 403:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5342 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 404:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5348 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 405:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5354 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 406:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5360 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 407:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5366 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 408:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5372 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 409:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5378 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 410:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 411:
#line 1745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5390 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 412:
#line 1746 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5396 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 413:
#line 1746 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5402 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 414:
#line 1750 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5411 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 415:
#line 1757 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5420 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 416:
#line 1763 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5428 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 417:
#line 1768 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5436 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 418:
#line 1773 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5445 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 419:
#line 1779 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5453 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 420:
#line 1784 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5461 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 421:
#line 1789 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5469 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 422:
#line 1794 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 423:
#line 1800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5486 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 424:
#line 1805 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5495 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 425:
#line 1811 "src/mongo/db/cst/grammar.yy"
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
#line 5507 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 426:
#line 1820 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5516 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 427:
#line 1826 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5525 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 428:
#line 1832 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5533 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 429:
#line 1837 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 430:
#line 1843 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5551 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 431:
#line 1849 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5559 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 432:
#line 1854 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5567 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 433:
#line 1859 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5575 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 434:
#line 1864 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5583 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 435:
#line 1869 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5591 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 436:
#line 1874 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5599 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 437:
#line 1879 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5607 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 438:
#line 1884 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5615 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 439:
#line 1889 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5623 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 440:
#line 1894 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5631 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 441:
#line 1899 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5639 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 442:
#line 1904 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5647 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 443:
#line 1909 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5655 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 444:
#line 1914 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5663 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 445:
#line 1920 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5669 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 446:
#line 1920 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5675 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 447:
#line 1920 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5681 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 448:
#line 1924 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5690 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 449:
#line 1931 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5699 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 450:
#line 1938 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5708 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 451:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5714 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 452:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5720 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 453:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 454:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5732 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 455:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5738 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 456:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5744 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 457:
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 458:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5756 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 459:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5762 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 460:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5768 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 461:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 462:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5780 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 463:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5786 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 464:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 465:
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5798 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 466:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5804 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 467:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5810 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 468:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5816 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 469:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 470:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5828 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 471:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5834 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 472:
#line 1947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5840 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 473:
#line 1951 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5852 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 474:
#line 1961 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5860 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 475:
#line 1964 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5868 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 476:
#line 1970 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5876 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 477:
#line 1973 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5884 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 478:
#line 1981 "src/mongo/db/cst/grammar.yy"
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
#line 5894 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 479:
#line 1990 "src/mongo/db/cst/grammar.yy"
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
#line 5904 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 480:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5910 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 481:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5916 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 482:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5922 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 483:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5928 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 484:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5934 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 485:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5940 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 486:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5946 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 487:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5952 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 488:
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5958 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 489:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5964 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 490:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5970 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 491:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5976 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 492:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5982 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 493:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5988 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 494:
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5994 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 495:
#line 2003 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
                    }
#line 6002 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 496:
#line 2006 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::hourArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6010 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 497:
#line 2012 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
                    }
#line 6018 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 498:
#line 2015 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::minuteArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6026 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 499:
#line 2021 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
                    }
#line 6034 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 500:
#line 2024 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::secondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6042 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 501:
#line 2030 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
                    }
#line 6050 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 502:
#line 2033 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::millisecondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6058 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 503:
#line 2039 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
                    }
#line 6066 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 504:
#line 2042 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6074 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 505:
#line 2048 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 6082 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 506:
#line 2051 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoDayOfWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6090 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 507:
#line 2057 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 6098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 508:
#line 2060 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6106 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 509:
#line 2066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
                    }
#line 6114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 510:
#line 2069 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::iso8601Arg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6122 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 511:
#line 2075 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
                    }
#line 6130 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 512:
#line 2078 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::monthArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 513:
#line 2085 "src/mongo/db/cst/grammar.yy"
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
#line 6148 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 514:
#line 2091 "src/mongo/db/cst/grammar.yy"
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
#line 6158 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 515:
#line 2099 "src/mongo/db/cst/grammar.yy"
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
#line 6168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 516:
#line 2107 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6176 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 517:
#line 2110 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6185 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 518:
#line 2114 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6193 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 519:
#line 2120 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 520:
#line 2124 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6211 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 521:
#line 2128 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6219 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 522:
#line 2134 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6228 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 523:
#line 2138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6237 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 524:
#line 2142 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6245 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 525:
#line 2148 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6254 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 526:
#line 2152 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6263 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 527:
#line 2156 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6271 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 528:
#line 2162 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6280 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 529:
#line 2166 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6289 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 530:
#line 2170 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6297 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 531:
#line 2176 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 532:
#line 2180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6315 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 533:
#line 2184 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6323 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 534:
#line 2190 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6332 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 535:
#line 2194 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6341 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 536:
#line 2198 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6349 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 537:
#line 2204 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6358 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 538:
#line 2208 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6367 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 539:
#line 2212 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 540:
#line 2218 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 541:
#line 2222 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6393 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 542:
#line 2226 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6401 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 543:
#line 2232 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6410 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 544:
#line 2236 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6419 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 545:
#line 2240 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6427 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 546:
#line 2246 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6436 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 547:
#line 2250 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6445 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 548:
#line 2254 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6453 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 549:
#line 2260 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6462 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 550:
#line 2264 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6471 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 551:
#line 2268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6479 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 552:
#line 2274 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6488 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 553:
#line 2278 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6497 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 554:
#line 2282 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6505 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 555:
#line 2288 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 6513 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 556:
#line 2291 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6521 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 557:
#line 2294 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6529 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 558:
#line 2301 "src/mongo/db/cst/grammar.yy"
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
#line 6541 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 559:
#line 2312 "src/mongo/db/cst/grammar.yy"
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
#line 6553 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 560:
#line 2322 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 6561 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 561:
#line 2325 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6569 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 562:
#line 2331 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6579 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 563:
#line 2339 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6589 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 564:
#line 2347 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6599 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 565:
#line 2355 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 6607 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 566:
#line 2358 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6615 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 567:
#line 2363 "src/mongo/db/cst/grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 6627 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 568:
#line 2372 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6635 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 569:
#line 2378 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6643 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 570:
#line 2384 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6651 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 571:
#line 2391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6662 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 572:
#line 2401 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6673 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 573:
#line 2410 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6682 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 574:
#line 2417 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6691 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 575:
#line 2424 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6700 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 576:
#line 2432 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6709 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 577:
#line 2440 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6718 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 578:
#line 2448 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6727 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 579:
#line 2456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6736 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 580:
#line 2463 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6744 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 581:
#line 2469 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6752 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 582:
#line 2475 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 6760 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 583:
#line 2478 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 6768 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 584:
#line 2484 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6776 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 585:
#line 2490 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6784 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 586:
#line 2495 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 587:
#line 2498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6801 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 588:
#line 2505 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 6809 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 589:
#line 2508 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 6817 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 590:
#line 2511 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 6825 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 591:
#line 2514 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 6833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 592:
#line 2517 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 6841 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 593:
#line 2520 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 6849 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 594:
#line 2523 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 6857 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 595:
#line 2526 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 6865 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 596:
#line 2531 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 6873 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 597:
#line 2533 "src/mongo/db/cst/grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 6885 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 598:
#line 2543 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6893 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 599:
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6901 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 600:
#line 2551 "src/mongo/db/cst/grammar.yy"
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
#line 6922 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 601:
#line 2570 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6930 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 602:
#line 2573 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6939 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 603:
#line 2580 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6947 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 604:
#line 2583 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6955 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 605:
#line 2589 "src/mongo/db/cst/grammar.yy"
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
#line 6971 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 606:
#line 2603 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6977 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 607:
#line 2604 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6983 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 608:
#line 2605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6989 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 609:
#line 2606 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6995 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 610:
#line 2607 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7001 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 611:
#line 2611 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = {CNode::ObjectChildren{
                            {KeyFieldname::elemMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7009 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 612:
#line 2617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7017 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 613:
#line 2620 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7026 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 614:
#line 2628 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 7034 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 615:
#line 2635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7043 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 616:
#line 2639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7052 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 617:
#line 2647 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7060 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 618:
#line 2650 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7068 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 619:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7074 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 620:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7080 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 621:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7086 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 622:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7092 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 623:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 624:
#line 2656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7104 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 625:
#line 2657 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7110 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 626:
#line 2661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 7118 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 627:
#line 2667 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 7126 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 628:
#line 2673 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7135 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 629:
#line 2681 "src/mongo/db/cst/grammar.yy"
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
#line 7147 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 630:
#line 2692 "src/mongo/db/cst/grammar.yy"
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
#line 7159 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 631:
#line 2702 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 632:
#line 2710 "src/mongo/db/cst/grammar.yy"
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
#line 7180 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 633:
#line 2720 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 634:
#line 2720 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7192 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 635:
#line 2724 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7201 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 636:
#line 2731 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 637:
#line 2738 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7216 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 638:
#line 2738 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7222 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 639:
#line 2742 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7228 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 640:
#line 2742 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 641:
#line 2746 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 7242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 642:
#line 2752 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 7248 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 643:
#line 2753 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 7257 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 644:
#line 2760 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 7265 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 645:
#line 2766 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7273 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 646:
#line 2769 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 647:
#line 2776 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 648:
#line 2783 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7296 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 649:
#line 2784 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7302 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 650:
#line 2785 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7308 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 651:
#line 2786 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7314 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 652:
#line 2787 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7320 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 653:
#line 2788 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7326 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 654:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7332 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 655:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7338 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 656:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7344 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 657:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7350 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 658:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7356 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 659:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7362 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 660:
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7368 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 661:
#line 2793 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7377 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 662:
#line 2798 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7386 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 663:
#line 2803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7395 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 664:
#line 2808 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7404 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 665:
#line 2813 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7413 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 666:
#line 2818 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 667:
#line 2823 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7431 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 668:
#line 2829 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7437 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 669:
#line 2830 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7443 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 670:
#line 2831 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7449 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 671:
#line 2832 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7455 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 672:
#line 2833 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7461 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 673:
#line 2834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7467 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 674:
#line 2835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7473 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 675:
#line 2836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7479 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 676:
#line 2837 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7485 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 677:
#line 2838 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7491 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 678:
#line 2843 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 7499 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 679:
#line 2846 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7507 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 680:
#line 2853 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 7515 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 681:
#line 2856 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7523 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 682:
#line 2863 "src/mongo/db/cst/grammar.yy"
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
#line 7534 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 683:
#line 2872 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 684:
#line 2877 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 685:
#line 2882 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7558 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 686:
#line 2887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 687:
#line 2892 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7574 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 688:
#line 2897 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7582 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 689:
#line 2902 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 690:
#line 2907 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 691:
#line 2912 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7606 "src/mongo/db/cst/parser_gen.cpp"
                    break;


#line 7610 "src/mongo/db/cst/parser_gen.cpp"

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
                yyn = yypact_[yystack_[0].state];
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
            error_token.state = static_cast<state_type>(yyn);
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

void ParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

// Generate an error message.
std::string ParserGen::yysyntax_error_(state_type yystate, const symbol_type& yyla) const {
    // Number of reported tokens (one for the "unexpected", one per
    // "expected").
    std::ptrdiff_t yycount = 0;
    // Its maximum.
    enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
    // Arguments of yyformat.
    char const* yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];

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
    if (!yyla.empty()) {
        symbol_number_type yytoken = yyla.type_get();
        yyarg[yycount++] = yytname_[yytoken];

        int yyn = yypact_[yystate];
        if (!yy_pact_value_is_default_(yyn)) {
            /* Start YYX at -YYN if negative to avoid negative indexes in
               YYCHECK.  In other words, skip the first -YYN actions for
               this state because they are default actions.  */
            int yyxbegin = yyn < 0 ? -yyn : 0;
            // Stay within bounds of both yycheck and yytname.
            int yychecklim = yylast_ - yyn + 1;
            int yyxend = yychecklim < yyntokens_ ? yychecklim : yyntokens_;
            for (int yyx = yyxbegin; yyx < yyxend; ++yyx)
                if (yycheck_[yyx + yyn] == yyx && yyx != yy_error_token_ &&
                    !yy_table_value_is_error_(yytable_[yyx + yyn])) {
                    if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM) {
                        yycount = 1;
                        break;
                    } else
                        yyarg[yycount++] = yytname_[yyx];
                }
        }
    }

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
            yyres += yytnamerr_(yyarg[yyi++]);
            ++yyp;
        } else
            yyres += *yyp;
    return yyres;
}


const short ParserGen::yypact_ninf_ = -1089;

const short ParserGen::yytable_ninf_ = -506;

const short ParserGen::yypact_[] = {
    -58,   -119,  -116,  -108,  -101,  50,    -89,   -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -46,   -9,    2150,  269,   1285,  -82,   2485,  -116,  -75,   -71,   2485,
    -69,   12,    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, 3637,  -1089, 3789,  -1089, -1089, -1089, -69,   251,   -1089, -1089,
    -1089, -1089, 4245,  -1089, -1089, -1089, -1089, -1089, -56,   -1089, -1089, -1089, -1089,
    4397,  -1089, -1089, 4397,  -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, 39,    -1089, -1089, -1089, -1089, 27,    -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, 45,    -1089, -1089, 99,    -89,   -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, 1984,  -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    104,   -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, 1460,  -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, 14,    -1089, -1089, -1089, 2255,  2485,  317,   -1089,
    2421,  1810,  2573,  3941,  3941,  3941,  -20,   -17,   -20,   -15,   3941,  3941,  3941,
    -1,    3941,  3941,  -1,    0,     1,     -69,   3941,  3941,  -69,   -69,   -69,   -69,
    4093,  4093,  4093,  3941,  3,     -1,    3941,  3941,  -1,    -1,    4093,  10,    15,
    4093,  4093,  4093,  18,    3941,  24,    3941,  -1,    -1,    -69,   87,    4093,  4093,
    25,    4093,  36,    -1,    44,    -20,   48,    3941,  -69,   -69,   -69,   -69,   -69,
    51,    -69,   4093,  -1,    54,    56,    -1,    60,    3941,  3941,  62,    63,    3941,
    64,    3789,  3789,  68,    70,    71,    72,    3941,  3941,  3789,  3789,  3789,  3789,
    3789,  3789,  3789,  3789,  3789,  3789,  -69,   73,    3789,  4093,  4093,  2423,  40,
    118,   -23,   -116,  -116,  -1089, 952,   4397,  4397,  2323,  -1089, -111,  -1089, 4549,
    4549,  -1089, -1089, 77,    112,   -1089, -1089, -1089, 3637,  -1089, -1089, -1089, 3789,
    -1089, -1089, -1089, -1089, -1089, -1089, 81,    82,    84,    92,    3789,  100,   3789,
    119,   123,   147,   3789,  154,   155,   158,   159,   -1089, 3637,  165,   162,   163,
    223,   225,   227,   228,   1984,  -1089, -1089, 170,   171,   235,   178,   179,   241,
    181,   182,   244,   184,   3789,  185,   186,   187,   188,   189,   194,   195,   257,
    3789,  3789,  198,   200,   262,   202,   203,   265,   238,   239,   301,   3637,  242,
    3789,  243,   245,   246,   304,   247,   252,   256,   259,   260,   261,   267,   268,
    271,   272,   276,   305,   277,   278,   312,   3789,  279,   281,   323,   3789,  282,
    3789,  286,   3789,  288,   289,   343,   291,   292,   349,   355,   3789,  304,   300,
    306,   366,   309,   3789,  3789,  313,   3789,  315,   316,   3789,  318,   319,   3789,
    320,   3789,  322,   325,   3789,  3789,  3789,  3789,  326,   327,   328,   330,   332,
    334,   335,   337,   338,   339,   344,   345,   304,   3789,  346,   347,   348,   373,
    352,   357,   378,   -1089, 3789,  -1089, -1089, -1089, -1089, -1089, 40,    407,   -1089,
    3637,  295,   -109,  258,   -1089, -1089, -1089, -1089, -1089, 363,   374,   2485,  375,
    -1089, -1089, -1089, -1089, -1089, -1089, 382,   1635,  -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -14,   -1089, 384,   -1089, -1089, -1089, -1089, 385,   -1089, 386,   -1089,
    -1089, -1089, 3789,  -1089, -1089, -1089, -1089, 2725,  388,   3789,  -1089, -1089, 3789,
    427,   3789,  3789,  3789,  -1089, -1089, 3789,  -1089, -1089, 3789,  -1089, -1089, 3789,
    -1089, 3789,  -1089, -1089, -1089, -1089, -1089, -1089, -1089, 3789,  3789,  3789,  -1089,
    -1089, 3789,  -1089, -1089, 3789,  -1089, -1089, 3789,  389,   -1089, 3789,  -1089, -1089,
    -1089, 3789,  442,   -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, 3789,  -1089, -1089, 3789,  3789,  -1089, -1089, 3789,  3789,  -1089, 391,   -1089,
    3789,  -1089, -1089, 3789,  -1089, -1089, 3789,  3789,  3789,  445,   -1089, -1089, 3789,
    -1089, 3789,  3789,  -1089, 3789,  -1089, -1089, 3789,  -1089, -1089, 3789,  -1089, 3789,
    -1089, -1089, 3789,  3789,  3789,  3789,  -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, 448,   3789,  -1089, -1089, -1089, 3789,  -1089, -1089,
    3789,  -1089, -1089, 317,   435,   -1089, 2485,  -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, 2485,  -1089, -1089, 4549,  4549,  -1089, 2355,  398,   -1089, 399,
    401,   404,   405,   406,   451,   -1089, 3789,  93,    459,   460,   459,   444,   444,
    444,   411,   444,   3789,  3789,  444,   444,   444,   413,   412,   -1089, 3789,  444,
    444,   419,   444,   -1089, 420,   424,   461,   487,   488,   438,   3789,  444,   -1089,
    -1089, -1089, 2877,  439,   440,   3789,  3789,  3789,  441,   3789,  443,   444,   444,
    -1089, 317,   447,   2485,  -41,   4592,  449,   -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, 3789,  484,   -1089, 3789,  3789,  501,   506,   3789,  444,
    40,    444,   444,   3789,  462,   465,   471,   472,   478,   3789,  481,   497,   498,
    499,   500,   -1089, 502,   504,   509,   511,   512,   517,   3029,  -1089, 523,   3789,
    565,   3789,  3789,  526,   527,   530,   3181,  3333,  3485,  532,   540,   541,   547,
    552,   556,   557,   559,   561,   562,   566,   -1089, -1089, 568,   571,   -1089, -1089,
    572,   -1089, 3789,  495,   -1089, -1089, 3789,  610,   3789,  622,   -1089, 451,   -1089,
    576,   484,   -1089, 578,   579,   581,   -1089, 582,   -1089, 584,   586,   587,   592,
    593,   -1089, 594,   595,   596,   -1089, 597,   598,   -1089, -1089, 3789,  638,   639,
    -1089, 601,   602,   604,   606,   607,   -1089, -1089, -1089, 608,   611,   612,   -1089,
    613,   -1089, 616,   617,   -1089, -1089, -1089, -1089, 3789,  -1089, 3789,  646,   -1089,
    3789,  484,   618,   619,   -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, 620,   3789,  3789,  -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, 621,   -1089, 3789,  444,   657,   623,   -1089,
    626,   -1089, 627,   636,   637,   -1089, 655,   501,   640,   -1089, 641,   643,   -1089,
    3789,  610,   -1089, -1089, -1089, 644,   646,   648,   444,   -1089, 649,   650,   -1089};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   7,   2,   79,  3,   601, 4,   586, 5,   1,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  11,  12,  13,  14,  15,  16,  6,   109, 167,
    149, 138, 148, 145, 159, 168, 152, 146, 154, 141, 163, 160, 161, 162, 166, 158, 156, 164, 143,
    144, 151, 139, 150, 153, 165, 157, 140, 147, 142, 155, 0,   78,  0,   371, 111, 110, 0,   0,
    117, 115, 116, 114, 0,   136, 80,  81,  83,  82,  0,   84,  85,  86,  600, 0,   70,  72,  0,
    71,  137, 602, 193, 261, 264, 169, 247, 171, 248, 260, 263, 262, 170, 265, 194, 176, 209, 172,
    183, 255, 258, 210, 225, 211, 226, 212, 213, 214, 266, 195, 372, 585, 177, 196, 197, 178, 179,
    215, 227, 228, 216, 217, 218, 173, 198, 199, 200, 180, 181, 229, 230, 219, 220, 201, 221, 202,
    182, 175, 174, 203, 267, 231, 232, 233, 235, 234, 204, 236, 222, 249, 250, 251, 252, 253, 254,
    257, 205, 237, 206, 129, 132, 133, 134, 135, 131, 130, 240, 238, 239, 241, 242, 243, 207, 256,
    259, 184, 185, 186, 187, 188, 189, 244, 190, 191, 246, 245, 208, 192, 223, 224, 597, 649, 650,
    651, 648, 0,   652, 653, 596, 587, 0,   308, 307, 306, 304, 303, 302, 296, 295, 294, 300, 299,
    298, 293, 297, 301, 305, 20,  21,  22,  23,  25,  26,  28,  0,   24,  9,   0,   7,   310, 309,
    269, 270, 271, 272, 273, 274, 275, 276, 642, 645, 277, 268, 278, 279, 280, 281, 282, 283, 284,
    285, 286, 287, 288, 289, 290, 291, 292, 320, 321, 322, 323, 324, 329, 325, 326, 327, 330, 331,
    104, 311, 312, 314, 315, 316, 328, 317, 318, 319, 637, 638, 639, 640, 313, 332, 365, 334, 118,
    345, 336, 335, 346, 354, 374, 347, 445, 446, 447, 348, 633, 634, 351, 451, 452, 453, 454, 455,
    456, 457, 458, 459, 460, 461, 462, 463, 464, 465, 466, 467, 468, 469, 470, 472, 471, 349, 654,
    655, 656, 657, 658, 659, 660, 355, 480, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491,
    492, 493, 494, 350, 668, 669, 670, 671, 672, 673, 674, 675, 676, 677, 375, 376, 377, 378, 379,
    380, 381, 382, 383, 384, 385, 386, 387, 388, 389, 352, 619, 620, 621, 622, 623, 624, 625, 353,
    399, 400, 401, 402, 403, 404, 405, 406, 407, 409, 410, 411, 408, 412, 413, 337, 344, 120, 128,
    127, 90,  88,  87,  112, 64,  63,  60,  59,  62,  56,  55,  58,  48,  47,  50,  52,  51,  54,
    0,   49,  53,  57,  61,  43,  44,  45,  46,  65,  66,  67,  36,  37,  38,  39,  40,  41,  42,
    606, 68,  608, 603, 605, 609, 610, 607, 604, 595, 594, 593, 592, 589, 588, 591, 590, 0,   598,
    599, 18,  0,   0,   0,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   122, 0,   0,   0,
    373, 0,   0,   0,   0,   615, 0,   27,  0,   0,   69,  29,  0,   0,   641, 643, 644, 0,   646,
    360, 333, 0,   338, 342, 362, 339, 343, 363, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   332, 0,   0,   0,   0,   503, 0,   0,   0,   9,   340, 341, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   560, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   560, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   560, 0,   0,   0,   0,   0,   0,   0,   0,   364, 0,   369,
    368, 370, 366, 121, 0,   124, 89,  0,   0,   0,   0,   91,  92,  95,  93,  94,  113, 0,   0,
    0,   618, 617, 614, 616, 582, 583, 0,   0,   30,  32,  33,  34,  35,  31,  17,  0,   647, 0,
    416, 438, 441, 414, 0,   448, 0,   437, 440, 439, 0,   415, 442, 417, 661, 0,   0,   0,   432,
    435, 0,   495, 0,   0,   0,   518, 516, 0,   521, 519, 0,   527, 525, 0,   443, 0,   662, 419,
    420, 663, 664, 530, 528, 0,   0,   0,   524, 522, 0,   539, 537, 0,   542, 540, 0,   0,   421,
    0,   423, 665, 666, 0,   0,   390, 391, 392, 393, 394, 395, 396, 397, 398, 551, 549, 0,   554,
    552, 0,   0,   533, 531, 0,   0,   667, 0,   449, 0,   444, 568, 0,   569, 570, 0,   0,   0,
    0,   548, 546, 0,   628, 0,   0,   631, 0,   431, 434, 0,   358, 359, 0,   428, 0,   574, 575,
    0,   0,   0,   0,   433, 436, 683, 684, 685, 686, 687, 688, 580, 689, 690, 581, 0,   0,   691,
    536, 534, 0,   545, 543, 0,   367, 123, 0,   0,   96,  0,   90,  105, 98,  101, 103, 102, 100,
    108, 611, 0,   612, 584, 0,   0,   74,  0,   0,   361, 0,   0,   0,   0,   0,   678, 504, 0,
    501, 474, 509, 474, 476, 476, 476, 0,   476, 555, 555, 476, 476, 476, 0,   0,   561, 0,   476,
    476, 0,   476, 332, 0,   0,   565, 0,   0,   0,   0,   476, 332, 332, 332, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   476, 476, 125, 0,   0,   0,   0,   0,   0,   77,  76,  73,  75,  19,
    626, 627, 356, 473, 635, 0,   680, 496, 0,   0,   497, 507, 0,   476, 0,   476, 476, 0,   0,
    0,   0,   0,   0,   556, 0,   0,   0,   0,   0,   636, 0,   0,   0,   0,   0,   0,   0,   450,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   126, 119, 0,   91,  97,  99,  0,   679, 0,   0,   506, 502, 0,   511, 0,   0,
    475, 678, 510, 0,   680, 477, 0,   0,   0,   418, 0,   557, 0,   0,   0,   0,   0,   422, 0,
    0,   0,   424, 0,   0,   426, 566, 0,   0,   0,   427, 0,   0,   0,   0,   0,   357, 573, 576,
    0,   0,   0,   429, 0,   430, 0,   0,   107, 106, 613, 681, 0,   498, 0,   499, 508, 0,   680,
    0,   0,   517, 520, 526, 529, 558, 559, 523, 538, 541, 562, 550, 553, 532, 425, 0,   0,   0,
    563, 547, 629, 630, 632, 577, 578, 579, 564, 535, 544, 0,   512, 0,   476, 501, 0,   515, 0,
    567, 0,   0,   0,   500, 0,   497, 0,   479, 0,   0,   682, 0,   511, 478, 572, 571, 0,   499,
    0,   476, 513, 0,   0,   514};

const short ParserGen::yypgoto_[] = {
    -1089, 236,   -5,    -1089, -1089, -8,    -1089, -1089, 4,     -1089, 9,     -1089, -762,
    230,   -1089, -1089, -233,  -1089, -1089, -12,   -50,   -74,   -42,   -33,   -10,   -31,
    8,     -11,   17,    -29,   -2,    -427,  -72,   -1089, 11,    19,    21,    -580,  30,
    46,    -60,   609,   -1089, -1089, -1089, -1089, -1089, -1089, -293,  -1089, 483,   -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, 131,   -919,  -574,  -1089,
    -13,   -70,   -285,  -1089, -1089, -64,   686,   -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -420,  -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -371,  -1088, -238,  -906,  -668,  -1089, -1089, -468,  -479,  -456,  -1089, -1089,
    -1089, -471,  -1089, -630,  -1089, -239,  -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -332,  -48,   -93,   4264,  685,   -6,    -1089,
    -202,  -1089, -1089, -1089, -1089, -1089, -276,  -1089, -1089, -1089, -1089, -1089, -1089,
    -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, -1089, 659,   -466,  -1089,
    -1089, -1089, -1089, -1089, 156,   -1089, -1089, -1089, -1089, -1089, -1089, -1089, 199};

const short ParserGen::yydefgoto_[] = {
    -1,   955,  599, 758, 207,  208,  95,   209,  210, 211,  212, 213,  592,  214,  80,  600,  957,
    762,  607,  96,  274, 275,  276,  277,  278,  279, 280,  281, 282,  283,  284,  285, 286,  287,
    288,  289,  290, 291, 292,  293,  294,  302,  296, 297,  298, 482,  299,  947,  948, 7,    16,
    27,   28,   29,  30,  31,   32,   33,   34,   477, 958,  788, 789,  456,  791,  949, 609,  628,
    726,  304,  305, 306, 583,  307,  308,  309,  310, 311,  312, 313,  314,  315,  316, 317,  318,
    319,  320,  321, 322, 323,  324,  325,  326,  706, 327,  328, 329,  330,  331,  332, 333,  334,
    335,  336,  337, 338, 339,  340,  341,  342,  343, 344,  345, 346,  347,  348,  349, 350,  351,
    352,  353,  354, 355, 356,  357,  358,  359,  360, 361,  362, 363,  364,  365,  366, 367,  368,
    369,  370,  371, 372, 373,  374,  375,  376,  377, 378,  379, 380,  381,  382,  383, 384,  385,
    386,  387,  388, 389, 1032, 1095, 1039, 1044, 860, 1066, 969, 1099, 1192, 1036, 819, 1101, 1041,
    1155, 1037, 483, 481, 1050, 390,  391,  392,  393, 394,  395, 396,  397,  398,  399, 400,  401,
    402,  403,  404, 405, 406,  407,  408,  409,  410, 411,  412, 413,  613,  614,  414, 415,  616,
    617,  645,  9,   17,  586,  421,  587,  82,   83,  84,   771, 772,  773,  774,  775, 85,   86,
    1019, 87,   88,  89,  585,  765,  940,  1016, 11,  18,   459, 460,  461,  462,  463, 593,  97,
    594,  13,   19,  474, 475,  786,  215,  5,    707};

const short ParserGen::yytable_[] = {
    303,  417,  813,  418,  763,  81,   784,  81,   237,  77,   235,  233,  241,  94,   235,  233,
    238,  443,  1160, 449,  443,  78,   449,  790,  790,  956,  79,   457,  6,    234,  457,  455,
    8,    234,  455,  767,  1021, 1022, 236,  943,  10,   442,  236,  458,  442,  893,  458,  12,
    785,  444,  14,   766,  444,  767,  604,  220,  221,  222,  445,  15,   446,  445,  447,  446,
    35,   447,  216,  768,  1045, 1046, 1194, 1048, 769,  239,  1052, 1053, 1054, 240,  266,  242,
    928,  1058, 1059, 478,  1061, 768,  244,  245,  246,  448,  769,  422,  448,  1071, 20,   21,
    22,   23,   24,   25,   26,   476,  450,  465,  466,  450,  1084, 1085, 467,  468,  451,  479,
    452,  451,  584,  452,  1034, -505, -505, 595,  1035, 453,  708,  709,  453,  780,  781,  300,
    469,  470,  621,  770,  623,  1103, 764,  1105, 1106, 454,  471,  472,  454,  1,    2,    3,
    4,    795,  627,  632,  633,  770,  654,  794,  615,  615,  615,  798,  799,  663,  800,  615,
    615,  615,  664,  615,  615,  674,  801,  681,  682,  615,  615,  676,  696,  683,  803,  643,
    643,  643,  615,  796,  231,  615,  615,  700,  938,  643,  815,  473,  643,  643,  643,  702,
    615,  805,  615,  704,  956,  806,  712,  643,  643,  718,  643,  719,  684,  685,  814,  721,
    615,  724,  727,  729,  631,  686,  687,  732,  643,  733,  734,  735,  749,  807,  615,  615,
    688,  243,  615,  655,  809,  810,  658,  659,  811,  812,  615,  615,  816,  817,  818,  820,
    821,  822,  678,  679,  823,  824,  689,  853,  825,  643,  643,  701,  826,  827,  828,  829,
    830,  831,  832,  834,  835,  836,  837,  838,  717,  792,  792,  720,  839,  840,  841,  725,
    844,  416,  845,  846,  847,  848,  849,  37,   38,   39,   40,   41,   42,   43,   1202, 44,
    45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    61,   62,   63,   64,   65,   66,   67,   1219, 850,  851,  852,  859,  854,  856,  872,  857,
    858,  861,  217,  218,  219,  875,  862,  220,  221,  222,  863,  247,  248,  864,  865,  866,
    879,  249,  247,  248,  941,  867,  868,  90,   249,  869,  870,  223,  224,  225,  871,  873,
    874,  877,  91,   878,  881,  226,  227,  228,  883,  1062, 885,  886,  887,  888,  889,  890,
    250,  251,  1072, 1073, 1074, 891,  894,  250,  251,  252,  253,  896,  895,  790,  790,  897,
    252,  253,  933,  900,  254,  902,  903,  936,  905,  906,  908,  254,  910,  247,  248,  911,
    916,  917,  918,  249,  919,  945,  920,  602,  921,  922,  257,  923,  924,  925,  644,  647,
    650,  257,  926,  927,  930,  931,  932,  620,  660,  622,  934,  665,  668,  671,  258,  935,
    591,  939,  250,  251,  950,  258,  690,  693,  268,  697,  942,  252,  253,  92,   93,   968,
    951,  953,  229,  230,  231,  232,  254,  714,  954,  960,  961,  962,  1104, 965,  983,  986,
    992,  601,  998,  235,  233,  1010, 81,   1015, 1025, 1026, 703,  1027, 257,  1028, 1038, 1029,
    1030, 1031, 1040, 1043, 1047, 1056, 234,  1055, 751,  754,  730,  731,  1060, 1065, 1063, 236,
    258,  1064, 738,  739,  740,  741,  742,  743,  744,  745,  746,  747,  1067, 1068, 750,  1069,
    1076, 1077, 1081, 1094, 1083, 443,  443,  449,  449,  1087, 1092, 443,  443,  449,  449,  457,
    457,  1098, 1100, 455,  455,  457,  457,  1152, 1108, 455,  455,  1109, 797,  442,  442,  458,
    458,  1110, 1111, 442,  442,  444,  444,  802,  1112, 804,  1114, 444,  444,  808,  445,  445,
    446,  446,  447,  447,  445,  445,  446,  446,  447,  447,  1115, 81,   1116, 1117, 1118, 759,
    1119, 779,  1120, 235,  233,  776,  777,  1121, 833,  1122, 1123, 760,  591,  448,  448,  1124,
    761,  842,  843,  448,  448,  1126, 234,  1128, 1131, 1132, 450,  450,  1133, 1137, 855,  236,
    450,  450,  451,  451,  452,  452,  1138, 1139, 451,  451,  452,  452,  1140, 453,  453,  792,
    792,  1141, 876,  453,  453,  1142, 880,  1143, 882,  1144, 884,  1145, 1146, 454,  454,  1154,
    1147, 1148, 892,  454,  454,  1149, 1150, 1157, 898,  899,  1159, 901,  1161, 1162, 904,  1163,
    1164, 907,  1165, 909,  1166, 1167, 912,  913,  914,  915,  1168, 1169, 1170, 1171, 1172, 1173,
    1174, 1176, 1177, 1178, 1179, 295,  1180, 929,  1181, 1182, 1183, 1191, 1035, 1184, 1185, 1186,
    937,  420,  1187, 1188, 1195, 1196, 1197, 1200, 1209, 1204, 946,  634,  1205, 1206, 637,  638,
    639,  640,  646,  649,  652,  944,  1207, 1208, 606,  598,  1211, 1212, 662,  1213, 1216, 667,
    670,  673,  1218, 1220, 1221, 1024, 1091, 480,  680,  793,  692,  695,  1158, 699,  1042, 1210,
    1217, 1203, 963,  1215, 1051, 1018, 1089, 710,  711,  966,  713,  716,  967,  783,  970,  971,
    972,  464,  0,    973,  0,    0,    974,  235,  233,  975,  0,    976,  0,    952,  0,    235,
    233,  0,    0,    977,  978,  979,  0,    0,    980,  748,  234,  981,  753,  756,  982,  0,
    959,  984,  234,  236,  0,    985,  0,    0,    0,    0,    0,    236,  0,    0,    0,    0,
    0,    0,    987,  0,    0,    988,  989,  0,    0,    990,  991,  0,    0,    0,    993,  0,
    0,    994,  0,    0,    995,  996,  997,  0,    0,    0,    999,  0,    1000, 1001, 0,    1002,
    0,    0,    1003, 0,    0,    1004, 0,    1005, 0,    0,    1006, 1007, 1008, 1009, 0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1011, 0,    0,    0,    1012,
    0,    0,    1013, 1014, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    443,  443,  449,  449,  0,    0,    0,    0,    0,    0,    457,  457,  0,    0,    455,
    455,  0,    1033, 0,    0,    0,    0,    0,    0,    442,  442,  0,    1049, 1049, 0,    0,
    0,    444,  444,  0,    1057, 0,    0,    0,    0,    0,    445,  445,  446,  446,  447,  447,
    1070, 1017, 0,    235,  233,  1075, 0,    0,    1078, 1079, 1080, 1020, 1082, 235,  233,  1086,
    0,    0,    0,    946,  0,    0,    234,  0,    0,    448,  448,  0,    0,    0,    0,    236,
    234,  1093, 0,    0,    1096, 1097, 450,  450,  1102, 236,  0,    0,    0,    1107, 451,  451,
    452,  452,  0,    1113, 0,    0,    0,    0,    0,    453,  453,  0,    0,    0,    0,    0,
    0,    0,    0,    1127, 0,    1129, 1130, 0,    0,    454,  454,  0,    1088, 0,    235,  233,
    235,  233,  0,    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,
    1151, 234,  0,    234,  1153, 0,    1156, 0,    0,    0,    236,  0,    236,  0,    0,    0,
    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,
    0,    0,    1175, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1189, 0,    1190, 0,    0,    1193,
    0,    0,    295,  0,    0,    611,  611,  611,  0,    0,    0,    778,  611,  611,  611,  0,
    611,  611,  1198, 1199, 0,    0,    611,  611,  0,    0,    0,    0,    642,  642,  642,  611,
    0,    1201, 611,  611,  0,    0,    642,  0,    0,    642,  642,  642,  0,    611,  0,    611,
    0,    0,    0,    1214, 642,  642,  0,    642,  229,  230,  231,  232,  0,    611,  0,    0,
    0,    0,    0,    0,    0,    642,  0,    0,    0,    0,    0,    611,  611,  0,    0,    611,
    0,    0,    612,  612,  612,  0,    0,    611,  611,  612,  612,  612,  0,    612,  612,  0,
    0,    0,    0,    612,  612,  0,    642,  642,  0,    648,  651,  0,    612,  0,    0,    612,
    612,  0,    661,  0,    0,    666,  669,  672,  0,    0,    612,  0,    612,  0,    0,    295,
    691,  694,  0,    698,  0,    0,    0,    0,    0,    0,    612,  0,    0,    0,    0,    0,
    0,    715,  0,    0,    0,    0,    0,    0,    612,  612,  295,  0,    612,  0,    0,    0,
    0,    0,    0,    0,    612,  612,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    752,  755,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    295,  0,    0,    0,    0,    98,   99,   100,  101,  102,  103,  104,  37,
    38,   39,   40,   41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,   52,
    53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   105,
    106,  107,  108,  109,  0,    0,    110,  111,  0,    112,  113,  114,  115,  116,  117,  118,
    119,  120,  121,  122,  123,  0,    0,    0,    124,  125,  0,    0,    0,    126,  0,    127,
    128,  0,    129,  0,    130,  0,    0,    131,  132,  133,  71,   0,    134,  135,  0,    0,
    295,  136,  137,  138,  139,  140,  141,  142,  0,    0,    0,    143,  144,  145,  146,  147,
    148,  149,  150,  151,  152,  0,    153,  154,  155,  156,  0,    0,    157,  158,  159,  160,
    161,  162,  163,  0,    0,    164,  165,  166,  167,  168,  169,  170,  171,  172,  0,    173,
    174,  175,  176,  177,  178,  179,  180,  181,  0,    0,    182,  183,  184,  185,  186,  187,
    188,  189,  190,  0,    0,    191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,
    202,  203,  204,  0,    205,  76,   206,  484,  485,  486,  487,  488,  489,  490,  37,   38,
    39,   40,   41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,
    54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   491,  492,
    493,  494,  495,  0,    0,    496,  497,  0,    498,  499,  500,  501,  502,  503,  504,  505,
    506,  507,  508,  509,  0,    0,    0,    510,  511,  0,    0,    0,    588,  0,    0,    512,
    0,    513,  0,    514,  0,    0,    515,  516,  517,  589,  0,    518,  519,  0,    0,    0,
    520,  521,  522,  523,  524,  525,  526,  0,    0,    0,    527,  528,  529,  530,  531,  532,
    533,  534,  535,  536,  0,    537,  538,  539,  540,  0,    0,    541,  542,  543,  544,  545,
    546,  547,  0,    0,    548,  549,  550,  551,  552,  553,  554,  555,  590,  0,    557,  558,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    559,  560,  561,  562,  563,  564,  565,
    566,  567,  0,    0,    568,  569,  570,  571,  572,  573,  574,  575,  576,  577,  578,  579,
    580,  581,  0,    582,  92,   93,   484,  485,  486,  487,  488,  489,  490,  37,   38,   39,
    40,   41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   491,  492,  493,
    494,  495,  0,    0,    496,  497,  0,    498,  499,  500,  501,  502,  503,  504,  505,  506,
    507,  508,  509,  0,    0,    0,    510,  511,  0,    0,    0,    0,    0,    0,    512,  0,
    513,  0,    514,  0,    0,    515,  516,  517,  589,  0,    518,  519,  0,    0,    0,    520,
    521,  522,  523,  524,  525,  526,  0,    0,    0,    527,  528,  529,  530,  531,  532,  533,
    534,  535,  536,  0,    537,  538,  539,  540,  0,    0,    541,  542,  543,  544,  545,  546,
    547,  0,    0,    548,  549,  550,  551,  552,  553,  554,  555,  556,  0,    557,  558,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    559,  560,  561,  562,  563,  564,  565,  566,
    567,  0,    0,    568,  569,  570,  571,  572,  573,  574,  575,  576,  577,  578,  579,  580,
    581,  0,    582,  92,   93,   98,   99,   100,  101,  102,  103,  104,  37,   38,   39,   40,
    41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,
    56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   105,  106,  107,  108,
    109,  0,    0,    110,  111,  0,    112,  113,  114,  115,  116,  117,  118,  119,  120,  121,
    122,  123,  0,    0,    0,    124,  125,  0,    0,    0,    126,  0,    605,  128,  0,    129,
    0,    130,  0,    0,    131,  132,  133,  71,   0,    134,  135,  0,    0,    0,    136,  137,
    138,  139,  140,  141,  142,  0,    0,    0,    143,  144,  145,  146,  147,  148,  149,  150,
    151,  152,  0,    153,  154,  155,  156,  0,    0,    157,  158,  159,  160,  161,  162,  163,
    0,    0,    164,  165,  166,  167,  168,  169,  170,  171,  172,  0,    173,  174,  175,  176,
    177,  178,  179,  180,  181,  0,    0,    182,  183,  184,  185,  186,  187,  188,  189,  190,
    0,    0,    191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,
    0,    205,  76,   484,  485,  486,  487,  488,  489,  490,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    491,  492,  493,  494,  495,  0,
    0,    496,  497,  0,    498,  499,  500,  501,  502,  503,  504,  505,  506,  507,  508,  509,
    0,    0,    0,    510,  511,  0,    0,    0,    0,    0,    0,    512,  0,    513,  0,    514,
    0,    0,    515,  516,  517,  0,    0,    518,  519,  0,    0,    0,    520,  521,  522,  523,
    524,  525,  526,  0,    0,    0,    527,  528,  529,  530,  531,  532,  533,  534,  535,  536,
    0,    537,  538,  539,  540,  0,    0,    541,  542,  543,  544,  545,  546,  547,  0,    0,
    548,  549,  550,  551,  552,  553,  554,  555,  556,  0,    557,  558,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    559,  560,  561,  562,  563,  564,  565,  566,  567,  0,    0,
    568,  569,  570,  571,  572,  573,  574,  575,  576,  577,  578,  579,  580,  581,  36,   582,
    37,   38,   39,   40,   41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,
    52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    68,   0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    69,   0,    0,    0,    70,   0,    0,    0,    0,    0,    0,    71,   0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    72,   0,    73,   37,   38,   39,   40,   41,   42,   43,
    0,    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   61,   62,   63,   64,   65,   66,   67,   0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    74,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    75,   0,    76,   0,    0,    596,  0,    0,    0,    37,   38,   39,
    40,   41,   42,   43,   597,  44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   37,   38,   39,
    40,   41,   42,   43,   0,    44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   782,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    589,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1023, 0,    92,
    93,   37,   38,   39,   40,   41,   42,   43,   589,  44,   45,   46,   47,   48,   49,   50,
    51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,
    67,   0,    0,    0,    245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    603,  0,
    0,    757,  0,    92,   93,   247,  248,  0,    0,    0,    0,    249,  71,   0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    92,   93,   0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  217,  218,  219,  0,    0,    220,  221,  222,  0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    255,  256,  0,    0,    0,    0,    223,  224,
    225,  0,    0,    0,    257,  0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    76,   258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    608,  0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    229,  230,  231,  232,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  301,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    964,  0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    300,  301,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    1028, 0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  301,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    1125, 0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    300,  301,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    1134, 0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  301,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    1135, 0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    300,  301,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    1136, 0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  301,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    0,    0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    255,  256,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    0,    0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  301,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    0,    0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    610,  301,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  245,  246,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    217,  218,  219,
    0,    0,    220,  221,  222,  0,    0,    0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    223,  224,  225,  0,    0,    0,    0,    0,    0,    0,
    226,  227,  228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    610,  641,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  259,  260,  261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  229,  230,  231,
    232,  271,  272,  273,  245,  246,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    217,  218,  219,  0,    0,    220,  221,  222,  0,    0,    0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    223,  224,
    225,  0,    0,    0,    0,    0,    0,    0,    226,  227,  228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    250,  251,  0,    0,    0,    0,
    0,    0,    0,    252,  253,  0,    0,    0,    0,    0,    0,    0,    0,    0,    254,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    419,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  259,  260,  261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  229,  230,  231,  232,  271,  272,  273,  423,  424,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    425,  426,  427,
    0,    0,    428,  429,  430,  0,    0,    0,    0,    0,    0,    0,    0,    247,  248,  0,
    0,    0,    0,    249,  0,    0,    431,  432,  433,  0,    0,    0,    0,    0,    0,    0,
    434,  435,  436,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    254,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    300,  437,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    258,  0,    0,    261,  262,  263,  264,  265,  266,  267,  268,  269,  270,  438,  439,  440,
    441,  271,  272,  273,  423,  424,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    425,  426,  427,  0,    0,    428,  429,  430,  0,    0,    0,
    0,    0,    0,    0,    0,    247,  248,  0,    0,    0,    0,    249,  0,    0,    431,  432,
    433,  0,    0,    0,    0,    0,    0,    0,    434,  435,  436,  0,    0,    0,    0,    0,
    217,  218,  219,  0,    0,    220,  221,  222,  0,    1090, 250,  251,  0,    0,    0,    0,
    247,  248,  0,    252,  253,  0,    249,  0,    0,    223,  224,  225,  0,    0,    254,  0,
    0,    0,    0,    226,  227,  228,  0,    0,    300,  787,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  250,  251,  0,    0,    0,    0,    0,    0,    0,    252,  253,
    0,    0,    0,    0,    0,    0,    0,    0,    258,  254,  0,    261,  262,  263,  264,  265,
    266,  267,  268,  269,  270,  438,  439,  440,  441,  271,  272,  273,  0,    618,  619,  257,
    0,    0,    0,    624,  625,  626,  0,    629,  630,  0,    0,    0,    0,    635,  636,  0,
    0,    0,    0,    258,  0,    0,    653,  0,    0,    656,  657,  0,    0,    0,    0,    0,
    229,  230,  231,  232,  675,  0,    677,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    705,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    722,  723,  0,    0,    728,  0,    0,    0,    0,    0,    0,    0,    736,  737};

const short ParserGen::yycheck_[] = {
    70,   75,   632,  75,   584,  17,   117,  19,   21,   17,   21,   21,   25,   18,   25,   25,
    22,   91,   1106, 91,   94,   17,   94,   597,  598,  787,  17,   91,   147,  21,   94,   91,
    148,  25,   94,   76,   955,  956,  21,   148,  148,  91,   25,   91,   94,   713,  94,   148,
    159,  91,   0,    74,   94,   76,   481,  69,   70,   71,   91,   148,  91,   94,   91,   94,
    73,   94,   148,  108,  974,  975,  1158, 977,  113,  148,  980,  981,  982,  148,  187,  148,
    748,  987,  988,  38,   990,  108,  74,   47,   48,   91,   113,  147,  94,   999,  140,  141,
    142,  143,  144,  145,  146,  74,   91,   64,   65,   94,   1012, 1013, 69,   70,   91,   12,
    91,   94,   10,   94,   23,   24,   25,   105,  27,   91,   542,  543,  94,   591,  592,  147,
    89,   90,   147,  172,  147,  1039, 16,   1041, 1042, 91,   99,   100,  94,   199,  200,  201,
    202,  33,   147,  147,  147,  172,  147,  74,   484,  485,  486,  74,   74,   147,  74,   491,
    492,  493,  147,  495,  496,  147,  74,   80,   81,   501,  502,  147,  147,  86,   74,   507,
    508,  509,  510,  606,  194,  513,  514,  147,  764,  517,  21,   148,  520,  521,  522,  147,
    524,  74,   526,  147,  958,  74,   147,  531,  532,  147,  534,  147,  117,  118,  633,  147,
    540,  147,  147,  147,  497,  126,  127,  147,  548,  147,  147,  147,  147,  74,   554,  555,
    137,  26,   558,  512,  74,   74,   515,  516,  74,   74,   566,  567,  74,   74,   15,   14,
    13,   13,   527,  528,  74,   74,   159,  674,  13,   581,  582,  536,  74,   74,   13,   74,
    74,   13,   74,   74,   74,   74,   74,   74,   549,  597,  598,  552,  74,   74,   13,   556,
    74,   74,   74,   13,   74,   74,   13,   10,   11,   12,   13,   14,   15,   16,   1192, 18,
    19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,
    35,   36,   37,   38,   39,   40,   41,   1217, 74,   74,   13,   11,   74,   74,   13,   74,
    74,   74,   64,   65,   66,   13,   74,   69,   70,   71,   74,   80,   81,   74,   74,   74,
    13,   86,   80,   81,   767,  74,   74,   74,   86,   74,   74,   89,   90,   91,   74,   74,
    74,   74,   85,   74,   74,   99,   100,  101,  74,   991,  74,   74,   21,   74,   74,   18,
    117,  118,  1000, 1001, 1002, 18,   74,   117,  118,  126,  127,  13,   74,   955,  956,  74,
    126,  127,  13,   74,   137,  74,   74,   13,   74,   74,   74,   137,  74,   80,   81,   74,
    74,   74,   74,   86,   74,   147,  74,   479,  74,   74,   159,  74,   74,   74,   507,  508,
    509,  159,  74,   74,   74,   74,   74,   487,  517,  489,  74,   520,  521,  522,  179,  74,
    437,  26,   117,  118,  73,   179,  531,  532,  189,  534,  147,  126,  127,  176,  177,  20,
    74,   74,   192,  193,  194,  195,  137,  548,  74,   73,   73,   73,   1040, 73,   73,   21,
    73,   478,  21,   478,  478,  21,   482,  36,   74,   74,   538,  74,   159,  73,   19,   74,
    74,   30,   22,   39,   73,   73,   478,  74,   581,  582,  560,  561,  73,   32,   74,   478,
    179,  73,   568,  569,  570,  571,  572,  573,  574,  575,  576,  577,  21,   21,   580,  73,
    73,   73,   73,   31,   73,   591,  592,  591,  592,  74,   73,   597,  598,  597,  598,  591,
    592,  28,   24,   591,  592,  597,  598,  40,   74,   597,  598,  74,   610,  591,  592,  591,
    592,  74,   74,   597,  598,  591,  592,  621,  74,   623,  73,   597,  598,  627,  591,  592,
    591,  592,  591,  592,  597,  598,  597,  598,  597,  598,  73,   583,  74,   74,   74,   583,
    74,   590,  74,   590,  590,  587,  588,  74,   654,  74,   74,   583,  593,  591,  592,  74,
    583,  663,  664,  597,  598,  74,   590,  34,   74,   74,   591,  592,  74,   73,   676,  590,
    597,  598,  591,  592,  591,  592,  74,   74,   597,  598,  597,  598,  73,   591,  592,  955,
    956,  73,   696,  597,  598,  73,   700,  74,   702,  74,   704,  74,   74,   591,  592,  29,
    74,   73,   712,  597,  598,  74,   74,   25,   718,  719,  74,   721,  74,   74,   724,  74,
    74,   727,  74,   729,  74,   74,   732,  733,  734,  735,  74,   74,   74,   74,   74,   74,
    74,   35,   35,   74,   74,   68,   74,   749,  74,   74,   74,   37,   27,   74,   74,   74,
    758,  80,   74,   74,   74,   74,   74,   74,   41,   74,   770,  500,  74,   74,   503,  504,
    505,  506,  507,  508,  509,  769,  74,   74,   482,  477,  74,   74,   517,  74,   74,   520,
    521,  522,  74,   74,   74,   958,  1019, 244,  529,  598,  531,  532,  1103, 534,  972,  1203,
    1215, 1193, 808,  1210, 979,  943,  1018, 544,  545,  815,  547,  548,  818,  593,  820,  821,
    822,  94,   -1,   825,  -1,   -1,   828,  770,  770,  831,  -1,   833,  -1,   778,  -1,   778,
    778,  -1,   -1,   841,  842,  843,  -1,   -1,   846,  578,  770,  849,  581,  582,  852,  -1,
    795,  855,  778,  770,  -1,   859,  -1,   -1,   -1,   -1,   -1,   778,  -1,   -1,   -1,   -1,
    -1,   -1,   872,  -1,   -1,   875,  876,  -1,   -1,   879,  880,  -1,   -1,   -1,   884,  -1,
    -1,   887,  -1,   -1,   890,  891,  892,  -1,   -1,   -1,   896,  -1,   898,  899,  -1,   901,
    -1,   -1,   904,  -1,   -1,   907,  -1,   909,  -1,   -1,   912,  913,  914,  915,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   929,  -1,   -1,   -1,   933,
    -1,   -1,   936,  939,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   955,  956,  955,  956,  -1,   -1,   -1,   -1,   -1,   -1,   955,  956,  -1,   -1,   955,
    956,  -1,   968,  -1,   -1,   -1,   -1,   -1,   -1,   955,  956,  -1,   978,  979,  -1,   -1,
    -1,   955,  956,  -1,   986,  -1,   -1,   -1,   -1,   -1,   955,  956,  955,  956,  955,  956,
    998,  942,  -1,   942,  942,  1003, -1,   -1,   1006, 1007, 1008, 952,  1010, 952,  952,  1015,
    -1,   -1,   -1,   1019, -1,   -1,   942,  -1,   -1,   955,  956,  -1,   -1,   -1,   -1,   942,
    952,  1031, -1,   -1,   1034, 1035, 955,  956,  1038, 952,  -1,   -1,   -1,   1043, 955,  956,
    955,  956,  -1,   1049, -1,   -1,   -1,   -1,   -1,   955,  956,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   1065, -1,   1067, 1068, -1,   -1,   955,  956,  -1,   1017, -1,   1017, 1017,
    1019, 1019, -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,
    1094, 1017, -1,   1019, 1098, -1,   1100, -1,   -1,   -1,   1017, -1,   1019, -1,   -1,   -1,
    -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,
    -1,   -1,   1128, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   1152, -1,   1154, -1,   -1,   1157,
    -1,   -1,   481,  -1,   -1,   484,  485,  486,  -1,   -1,   -1,   147,  491,  492,  493,  -1,
    495,  496,  1176, 1177, -1,   -1,   501,  502,  -1,   -1,   -1,   -1,   507,  508,  509,  510,
    -1,   1191, 513,  514,  -1,   -1,   517,  -1,   -1,   520,  521,  522,  -1,   524,  -1,   526,
    -1,   -1,   -1,   1209, 531,  532,  -1,   534,  192,  193,  194,  195,  -1,   540,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   548,  -1,   -1,   -1,   -1,   -1,   554,  555,  -1,   -1,   558,
    -1,   -1,   484,  485,  486,  -1,   -1,   566,  567,  491,  492,  493,  -1,   495,  496,  -1,
    -1,   -1,   -1,   501,  502,  -1,   581,  582,  -1,   508,  509,  -1,   510,  -1,   -1,   513,
    514,  -1,   517,  -1,   -1,   520,  521,  522,  -1,   -1,   524,  -1,   526,  -1,   -1,   606,
    531,  532,  -1,   534,  -1,   -1,   -1,   -1,   -1,   -1,   540,  -1,   -1,   -1,   -1,   -1,
    -1,   548,  -1,   -1,   -1,   -1,   -1,   -1,   554,  555,  633,  -1,   558,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   566,  567,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   581,  582,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   674,  -1,   -1,   -1,   -1,   3,    4,    5,    6,    7,    8,    9,    10,
    11,   12,   13,   14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,
    27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,
    43,   44,   45,   46,   -1,   -1,   49,   50,   -1,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   61,   62,   63,   -1,   -1,   -1,   67,   68,   -1,   -1,   -1,   72,   -1,   74,
    75,   -1,   77,   -1,   79,   -1,   -1,   82,   83,   84,   85,   -1,   87,   88,   -1,   -1,
    767,  92,   93,   94,   95,   96,   97,   98,   -1,   -1,   -1,   102,  103,  104,  105,  106,
    107,  108,  109,  110,  111,  -1,   113,  114,  115,  116,  -1,   -1,   119,  120,  121,  122,
    123,  124,  125,  -1,   -1,   128,  129,  130,  131,  132,  133,  134,  135,  136,  -1,   138,
    139,  140,  141,  142,  143,  144,  145,  146,  -1,   -1,   149,  150,  151,  152,  153,  154,
    155,  156,  157,  -1,   -1,   160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  170,
    171,  172,  173,  -1,   175,  176,  177,  3,    4,    5,    6,    7,    8,    9,    10,   11,
    12,   13,   14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,
    28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,
    44,   45,   46,   -1,   -1,   49,   50,   -1,   52,   53,   54,   55,   56,   57,   58,   59,
    60,   61,   62,   63,   -1,   -1,   -1,   67,   68,   -1,   -1,   -1,   72,   -1,   -1,   75,
    -1,   77,   -1,   79,   -1,   -1,   82,   83,   84,   85,   -1,   87,   88,   -1,   -1,   -1,
    92,   93,   94,   95,   96,   97,   98,   -1,   -1,   -1,   102,  103,  104,  105,  106,  107,
    108,  109,  110,  111,  -1,   113,  114,  115,  116,  -1,   -1,   119,  120,  121,  122,  123,
    124,  125,  -1,   -1,   128,  129,  130,  131,  132,  133,  134,  135,  136,  -1,   138,  139,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   149,  150,  151,  152,  153,  154,  155,
    156,  157,  -1,   -1,   160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  170,  171,
    172,  173,  -1,   175,  176,  177,  3,    4,    5,    6,    7,    8,    9,    10,   11,   12,
    13,   14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,
    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,
    45,   46,   -1,   -1,   49,   50,   -1,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    61,   62,   63,   -1,   -1,   -1,   67,   68,   -1,   -1,   -1,   -1,   -1,   -1,   75,   -1,
    77,   -1,   79,   -1,   -1,   82,   83,   84,   85,   -1,   87,   88,   -1,   -1,   -1,   92,
    93,   94,   95,   96,   97,   98,   -1,   -1,   -1,   102,  103,  104,  105,  106,  107,  108,
    109,  110,  111,  -1,   113,  114,  115,  116,  -1,   -1,   119,  120,  121,  122,  123,  124,
    125,  -1,   -1,   128,  129,  130,  131,  132,  133,  134,  135,  136,  -1,   138,  139,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   149,  150,  151,  152,  153,  154,  155,  156,
    157,  -1,   -1,   160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  170,  171,  172,
    173,  -1,   175,  176,  177,  3,    4,    5,    6,    7,    8,    9,    10,   11,   12,   13,
    14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,
    30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,
    46,   -1,   -1,   49,   50,   -1,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,
    62,   63,   -1,   -1,   -1,   67,   68,   -1,   -1,   -1,   72,   -1,   74,   75,   -1,   77,
    -1,   79,   -1,   -1,   82,   83,   84,   85,   -1,   87,   88,   -1,   -1,   -1,   92,   93,
    94,   95,   96,   97,   98,   -1,   -1,   -1,   102,  103,  104,  105,  106,  107,  108,  109,
    110,  111,  -1,   113,  114,  115,  116,  -1,   -1,   119,  120,  121,  122,  123,  124,  125,
    -1,   -1,   128,  129,  130,  131,  132,  133,  134,  135,  136,  -1,   138,  139,  140,  141,
    142,  143,  144,  145,  146,  -1,   -1,   149,  150,  151,  152,  153,  154,  155,  156,  157,
    -1,   -1,   160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  170,  171,  172,  173,
    -1,   175,  176,  3,    4,    5,    6,    7,    8,    9,    -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   42,   43,   44,   45,   46,   -1,
    -1,   49,   50,   -1,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,
    -1,   -1,   -1,   67,   68,   -1,   -1,   -1,   -1,   -1,   -1,   75,   -1,   77,   -1,   79,
    -1,   -1,   82,   83,   84,   -1,   -1,   87,   88,   -1,   -1,   -1,   92,   93,   94,   95,
    96,   97,   98,   -1,   -1,   -1,   102,  103,  104,  105,  106,  107,  108,  109,  110,  111,
    -1,   113,  114,  115,  116,  -1,   -1,   119,  120,  121,  122,  123,  124,  125,  -1,   -1,
    128,  129,  130,  131,  132,  133,  134,  135,  136,  -1,   138,  139,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   149,  150,  151,  152,  153,  154,  155,  156,  157,  -1,   -1,
    160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  170,  171,  172,  173,  8,    175,
    10,   11,   12,   13,   14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,
    26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   51,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    74,   -1,   -1,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   85,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   112,  -1,   114,  10,   11,   12,   13,   14,   15,   16,
    -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,
    33,   34,   35,   36,   37,   38,   39,   40,   41,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   174,  -1,   176,  -1,   -1,   74,   -1,   -1,   -1,   10,   11,   12,
    13,   14,   15,   16,   85,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,
    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   10,   11,   12,
    13,   14,   15,   16,   -1,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,
    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   74,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   85,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   74,   -1,   176,
    177,  10,   11,   12,   13,   14,   15,   16,   85,   18,   19,   20,   21,   22,   23,   24,
    25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,
    41,   -1,   -1,   -1,   47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   73,   -1,
    -1,   74,   -1,   176,  177,  80,   81,   -1,   -1,   -1,   -1,   86,   85,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   176,  177,  -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   159,  -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   176,  179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   192,  193,  194,  195,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   73,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   73,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   73,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   117,  118,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   126,  127,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  180,  181,  182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  47,   48,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   64,   65,   66,
    -1,   -1,   69,   70,   71,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   80,   81,   -1,
    -1,   -1,   -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    99,   100,  101,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  148,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    179,  -1,   -1,   182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  47,   48,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   80,   81,   -1,   -1,   -1,   -1,   86,   -1,   -1,   89,   90,
    91,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   99,   100,  101,  -1,   -1,   -1,   -1,   -1,
    64,   65,   66,   -1,   -1,   69,   70,   71,   -1,   73,   117,  118,  -1,   -1,   -1,   -1,
    80,   81,   -1,   126,  127,  -1,   86,   -1,   -1,   89,   90,   91,   -1,   -1,   137,  -1,
    -1,   -1,   -1,   99,   100,  101,  -1,   -1,   147,  148,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   159,  117,  118,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   126,  127,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   179,  137,  -1,   182,  183,  184,  185,  186,
    187,  188,  189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  -1,   485,  486,  159,
    -1,   -1,   -1,   491,  492,  493,  -1,   495,  496,  -1,   -1,   -1,   -1,   501,  502,  -1,
    -1,   -1,   -1,   179,  -1,   -1,   510,  -1,   -1,   513,  514,  -1,   -1,   -1,   -1,   -1,
    192,  193,  194,  195,  524,  -1,   526,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   540,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   554,  555,  -1,   -1,   558,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   566,  567};

const short ParserGen::yystos_[] = {
    0,   199, 200, 201, 202, 448, 147, 252, 148, 409, 148, 432, 148, 442, 0,   148, 253, 410, 433,
    443, 140, 141, 142, 143, 144, 145, 146, 254, 255, 256, 257, 258, 259, 260, 261, 73,  8,   10,
    11,  12,  13,  14,  15,  16,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  51,  74,  78,  85,  112, 114, 158, 174,
    176, 208, 211, 213, 217, 222, 414, 415, 416, 422, 423, 425, 426, 427, 74,  85,  176, 177, 205,
    209, 222, 440, 3,   4,   5,   6,   7,   8,   9,   42,  43,  44,  45,  46,  49,  50,  52,  53,
    54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  67,  68,  72,  74,  75,  77,  79,  82,  83,
    84,  87,  88,  92,  93,  94,  95,  96,  97,  98,  102, 103, 104, 105, 106, 107, 108, 109, 110,
    111, 113, 114, 115, 116, 119, 120, 121, 122, 123, 124, 125, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 138, 139, 140, 141, 142, 143, 144, 145, 146, 149, 150, 151, 152, 153, 154, 155, 156,
    157, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 175, 177, 207, 208,
    210, 211, 212, 213, 214, 216, 447, 148, 64,  65,  66,  69,  70,  71,  89,  90,  91,  99,  100,
    101, 192, 193, 194, 195, 227, 229, 230, 231, 268, 409, 148, 148, 268, 148, 449, 74,  47,  48,
    80,  81,  86,  117, 118, 126, 127, 137, 147, 148, 159, 179, 180, 181, 182, 183, 184, 185, 186,
    187, 188, 189, 190, 191, 196, 197, 198, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233,
    234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 249, 147, 148, 244, 269,
    272, 273, 274, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291,
    292, 293, 294, 295, 297, 298, 299, 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311,
    312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330,
    331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 378, 379, 380, 381, 382, 383, 384, 385, 386,
    387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399, 400, 401, 404, 405, 449, 224,
    235, 148, 244, 412, 147, 47,  48,  64,  65,  66,  69,  70,  71,  89,  90,  91,  99,  100, 101,
    148, 192, 193, 194, 195, 223, 224, 225, 226, 228, 232, 233, 235, 237, 238, 239, 241, 242, 243,
    266, 273, 405, 434, 435, 436, 437, 438, 434, 64,  65,  69,  70,  89,  90,  99,  100, 148, 444,
    445, 74,  262, 38,  12,  253, 376, 248, 375, 3,   4,   5,   6,   7,   8,   9,   42,  43,  44,
    45,  46,  49,  50,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  67,  68,  75,
    77,  79,  82,  83,  84,  87,  88,  92,  93,  94,  95,  96,  97,  98,  102, 103, 104, 105, 106,
    107, 108, 109, 110, 111, 113, 114, 115, 116, 119, 120, 121, 122, 123, 124, 125, 128, 129, 130,
    131, 132, 133, 134, 135, 136, 138, 139, 149, 150, 151, 152, 153, 154, 155, 156, 157, 160, 161,
    162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 175, 275, 10,  428, 411, 413, 72,
    85,  136, 205, 215, 439, 441, 105, 74,  85,  204, 205, 218, 268, 235, 73,  234, 74,  216, 221,
    73,  269, 147, 244, 274, 402, 403, 404, 406, 407, 407, 407, 273, 147, 273, 147, 407, 407, 407,
    147, 270, 407, 407, 270, 147, 147, 449, 407, 407, 449, 449, 449, 449, 148, 244, 404, 406, 408,
    449, 406, 408, 449, 406, 408, 449, 407, 147, 270, 407, 407, 270, 270, 406, 408, 449, 147, 147,
    406, 408, 449, 406, 408, 449, 406, 408, 449, 147, 407, 147, 407, 270, 270, 449, 80,  81,  86,
    117, 118, 126, 127, 137, 159, 406, 408, 449, 406, 408, 449, 147, 406, 408, 449, 147, 270, 147,
    273, 147, 407, 296, 449, 296, 296, 449, 449, 147, 449, 406, 408, 449, 270, 147, 147, 270, 147,
    407, 407, 147, 270, 271, 147, 407, 147, 269, 269, 147, 147, 147, 147, 407, 407, 269, 269, 269,
    269, 269, 269, 269, 269, 269, 269, 449, 147, 269, 406, 408, 449, 406, 408, 449, 74,  206, 208,
    211, 213, 220, 240, 16,  429, 74,  76,  108, 113, 172, 417, 418, 419, 420, 421, 409, 409, 147,
    268, 435, 435, 74,  441, 117, 159, 446, 148, 264, 265, 266, 267, 404, 264, 74,  33,  234, 269,
    74,  74,  74,  74,  269, 74,  269, 74,  74,  74,  269, 74,  74,  74,  74,  375, 234, 21,  74,
    74,  15,  370, 14,  13,  13,  74,  74,  13,  74,  74,  13,  74,  74,  13,  74,  269, 74,  74,
    74,  74,  74,  74,  74,  13,  269, 269, 74,  74,  13,  74,  74,  13,  74,  74,  13,  234, 74,
    269, 74,  74,  74,  11,  364, 74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  13,  74,
    74,  13,  269, 74,  74,  13,  269, 74,  269, 74,  269, 74,  74,  21,  74,  74,  18,  18,  269,
    364, 74,  74,  13,  74,  269, 269, 74,  269, 74,  74,  269, 74,  74,  269, 74,  269, 74,  74,
    269, 269, 269, 269, 74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  364, 269, 74,
    74,  74,  13,  74,  74,  13,  269, 240, 26,  430, 234, 147, 148, 243, 147, 235, 250, 251, 268,
    73,  74,  268, 74,  74,  204, 215, 219, 263, 230, 73,  73,  73,  269, 73,  73,  269, 269, 20,
    366, 269, 269, 269, 269, 269, 269, 269, 269, 269, 269, 269, 269, 269, 73,  269, 269, 21,  269,
    269, 269, 269, 269, 73,  269, 269, 269, 269, 269, 21,  269, 269, 269, 269, 269, 269, 269, 269,
    269, 269, 269, 21,  269, 269, 269, 235, 36,  431, 268, 411, 424, 268, 265, 265, 74,  219, 74,
    74,  74,  73,  74,  74,  30,  360, 269, 23,  27,  369, 374, 19,  362, 22,  372, 362, 39,  363,
    363, 363, 73,  363, 269, 377, 377, 363, 363, 363, 74,  73,  269, 363, 363, 73,  363, 375, 74,
    73,  32,  365, 21,  21,  73,  269, 363, 375, 375, 375, 269, 73,  73,  269, 269, 269, 73,  269,
    73,  363, 363, 235, 74,  268, 417, 73,  251, 73,  269, 31,  361, 269, 269, 28,  367, 24,  371,
    269, 363, 240, 363, 363, 269, 74,  74,  74,  74,  74,  269, 73,  73,  74,  74,  74,  74,  74,
    74,  74,  74,  74,  73,  74,  269, 34,  269, 269, 74,  74,  74,  73,  73,  73,  73,  74,  74,
    73,  73,  73,  74,  74,  74,  74,  74,  73,  74,  74,  269, 40,  269, 29,  373, 269, 25,  360,
    74,  361, 74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  269, 35,  35,
    74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  74,  269, 269, 37,  368, 269, 361, 74,  74,
    74,  269, 269, 74,  269, 363, 369, 74,  74,  74,  74,  74,  41,  367, 74,  74,  74,  269, 373,
    74,  368, 74,  363, 74,  74};

const short ParserGen::yyr1_[] = {
    0,   203, 448, 448, 448, 448, 252, 253, 253, 449, 254, 254, 254, 254, 254, 254, 254, 261, 255,
    256, 268, 268, 268, 268, 257, 258, 259, 260, 262, 262, 218, 218, 264, 265, 265, 265, 266, 266,
    266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266,
    266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 266, 204, 205, 205, 205, 267, 263, 263,
    219, 219, 409, 410, 410, 414, 414, 414, 414, 414, 414, 415, 412, 412, 411, 411, 417, 417, 417,
    417, 420, 250, 424, 424, 251, 251, 421, 421, 422, 418, 418, 419, 416, 423, 423, 423, 413, 413,
    217, 217, 217, 211, 425, 426, 428, 428, 429, 429, 430, 430, 431, 427, 427, 207, 207, 207, 207,
    207, 207, 207, 208, 209, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222,
    222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 210, 210,
    210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 235, 235, 235, 235, 235, 235, 235, 235, 235, 235, 236, 249, 237, 238, 239, 241, 242,
    243, 223, 224, 225, 226, 228, 232, 233, 227, 227, 227, 227, 229, 229, 229, 229, 230, 230, 230,
    230, 231, 231, 231, 231, 240, 240, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244,
    244, 244, 244, 244, 244, 244, 244, 244, 244, 375, 375, 269, 269, 269, 269, 402, 402, 408, 408,
    403, 403, 404, 404, 405, 405, 405, 405, 405, 405, 405, 405, 405, 405, 270, 271, 272, 272, 273,
    406, 407, 407, 274, 275, 275, 220, 206, 206, 206, 213, 214, 215, 276, 276, 276, 276, 276, 276,
    276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 277, 277, 277, 277, 277, 277, 277, 277, 277,
    386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 386, 278, 399, 345, 346,
    347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 387, 388, 389, 390, 391, 392,
    393, 394, 395, 396, 397, 398, 400, 401, 279, 279, 279, 280, 281, 282, 286, 286, 286, 286, 286,
    286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 287, 362,
    362, 363, 363, 288, 289, 318, 318, 318, 318, 318, 318, 318, 318, 318, 318, 318, 318, 318, 318,
    318, 366, 366, 367, 367, 368, 368, 369, 369, 370, 370, 374, 374, 371, 371, 372, 372, 373, 373,
    319, 319, 320, 321, 321, 321, 322, 322, 322, 325, 325, 325, 323, 323, 323, 324, 324, 324, 330,
    330, 330, 332, 332, 332, 326, 326, 326, 327, 327, 327, 333, 333, 333, 331, 331, 331, 328, 328,
    328, 329, 329, 329, 377, 377, 377, 290, 291, 364, 364, 292, 299, 309, 365, 365, 296, 293, 294,
    295, 297, 298, 300, 301, 302, 303, 304, 305, 306, 307, 308, 446, 446, 444, 442, 443, 443, 445,
    445, 445, 445, 445, 445, 445, 445, 212, 212, 447, 447, 432, 433, 433, 440, 440, 434, 435, 435,
    435, 435, 435, 437, 436, 436, 438, 439, 439, 441, 441, 378, 378, 378, 378, 378, 378, 378, 379,
    380, 381, 382, 383, 384, 385, 283, 283, 284, 285, 234, 234, 245, 245, 246, 376, 376, 247, 248,
    248, 221, 216, 216, 216, 216, 216, 216, 310, 310, 310, 310, 310, 310, 310, 311, 312, 313, 314,
    315, 316, 317, 334, 334, 334, 334, 334, 334, 334, 334, 334, 334, 360, 360, 361, 361, 335, 336,
    337, 338, 339, 340, 341, 342, 343, 344};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 2, 3, 0, 4, 1, 1, 1, 1, 1, 1,  1,  1, 5, 3, 7, 1,  1,  1, 1, 2, 2, 2, 4, 0,
    2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  3,  1, 2, 2, 2, 3,  0,  2, 1, 1, 1, 1, 1, 1,
    2, 1, 3, 0, 2, 1, 1, 1, 1, 2, 3, 0, 2, 1, 1,  2,  2, 2, 2, 5, 5,  5,  1, 1, 1, 0, 2, 1, 1,
    1, 1, 2, 7, 0, 2, 0, 2, 0, 2, 2, 2, 2, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 4, 5, 4, 4, 3, 3, 1,  1,  3, 0, 2, 2, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4,  4,  4, 4, 4, 4, 4,  4,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4,  4,  7, 4, 7, 8, 7,  7,  4, 7, 7, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 4, 4,  6,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1,
    1, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0,  2,  0, 2, 0, 2, 14, 16, 9, 4, 8, 4, 4, 8, 4,
    4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4,  4,  8, 4, 4, 8, 4,  4,  8, 4, 4, 8, 4, 4, 8,
    4, 4, 8, 4, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8, 0,  2,  7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8, 8, 8,
    4, 4, 1, 1, 4, 3, 0, 2, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 2, 2, 3,  0,  2, 2, 2, 1, 1, 1, 1,
    1, 1, 4, 4, 7, 3, 1, 2, 2, 2, 1, 1, 1, 1, 1,  1,  1, 6, 6, 4, 8,  8,  4, 8, 1, 1, 6, 6, 1,
    1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 4, 4, 4, 4, 4, 4,
    4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4,  4,  4, 4, 4};


// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a yyntokens_, nonterminals.
const char* const ParserGen::yytname_[] = {"\"EOF\"",
                                           "error",
                                           "$undefined",
                                           "ABS",
                                           "ACOS",
                                           "ACOSH",
                                           "ADD",
                                           "\"allElementsTrue\"",
                                           "AND",
                                           "\"anyElementTrue\"",
                                           "\"$caseSensitive argument\"",
                                           "\"chars argument\"",
                                           "\"coll argument\"",
                                           "\"date argument\"",
                                           "\"dateString argument\"",
                                           "\"day argument\"",
                                           "\"$diacriticSensitive argument\"",
                                           "\"filter\"",
                                           "\"find argument\"",
                                           "\"format argument\"",
                                           "\"hour argument\"",
                                           "\"input argument\"",
                                           "\"ISO 8601 argument\"",
                                           "\"ISO day of week argument\"",
                                           "\"ISO week argument\"",
                                           "\"ISO week year argument\"",
                                           "\"$language argument\"",
                                           "\"millisecond argument\"",
                                           "\"minute argument\"",
                                           "\"month argument\"",
                                           "\"onError argument\"",
                                           "\"onNull argument\"",
                                           "\"options argument\"",
                                           "\"pipeline argument\"",
                                           "\"regex argument\"",
                                           "\"replacement argument\"",
                                           "\"$search argument\"",
                                           "\"second argument\"",
                                           "\"size argument\"",
                                           "\"timezone argument\"",
                                           "\"to argument\"",
                                           "\"year argument\"",
                                           "ASIN",
                                           "ASINH",
                                           "ATAN",
                                           "ATAN2",
                                           "ATANH",
                                           "\"false\"",
                                           "\"true\"",
                                           "CEIL",
                                           "CMP",
                                           "COMMENT",
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
                                           "\"elemMatch operator\"",
                                           "\"end of array\"",
                                           "\"end of object\"",
                                           "EQ",
                                           "EXISTS",
                                           "EXPONENT",
                                           "EXPR",
                                           "FLOOR",
                                           "\"geoNearDistance\"",
                                           "\"geoNearPoint\"",
                                           "GT",
                                           "GTE",
                                           "HOUR",
                                           "ID",
                                           "\"indexKey\"",
                                           "INDEX_OF_BYTES",
                                           "INDEX_OF_CP",
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
                                           "SIN",
                                           "SINH",
                                           "\"slice\"",
                                           "\"sortKey\"",
                                           "SPLIT",
                                           "SQRT",
                                           "STAGE_INHIBIT_OPTIMIZATION",
                                           "STAGE_LIMIT",
                                           "STAGE_MATCH",
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
                                           "TEXT",
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
                                           "WHERE",
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
                                           "matchStage",
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
                                           "matchExpression",
                                           "predicates",
                                           "compoundMatchExprs",
                                           "predValue",
                                           "additionalExprs",
                                           "predicate",
                                           "fieldPredicate",
                                           "logicalExpr",
                                           "operatorExpression",
                                           "notExpr",
                                           "matchMod",
                                           "existsExpr",
                                           "typeExpr",
                                           "commentExpr",
                                           "logicalExprField",
                                           "typeValues",
                                           "matchExpr",
                                           "matchText",
                                           "matchWhere",
                                           "textArgCaseSensitive",
                                           "textArgDiacriticSensitive",
                                           "textArgLanguage",
                                           "textArgSearch",
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

#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    405,  405,  408,  411,  414,  421,  427,  428,  436,  439,  439,  439,  439,  439,  439,
    439,  442,  452,  458,  468,  468,  468,  468,  472,  477,  482,  488,  507,  510,  517,  520,
    526,  540,  541,  542,  546,  547,  548,  549,  550,  551,  552,  553,  554,  555,  556,  557,
    560,  563,  566,  569,  572,  575,  578,  581,  584,  587,  590,  593,  596,  599,  602,  605,
    608,  611,  612,  613,  614,  619,  628,  639,  640,  655,  662,  666,  674,  677,  683,  689,
    692,  699,  700,  703,  704,  705,  706,  709,  718,  719,  725,  728,  736,  736,  736,  736,
    740,  746,  752,  753,  760,  760,  764,  773,  783,  789,  794,  803,  813,  821,  822,  823,
    826,  829,  836,  836,  836,  839,  845,  851,  870,  873,  878,  881,  886,  889,  894,  900,
    901,  907,  910,  913,  916,  919,  922,  925,  931,  937,  953,  956,  959,  962,  965,  968,
    971,  974,  977,  980,  983,  986,  989,  992,  995,  998,  1001, 1004, 1007, 1010, 1013, 1016,
    1019, 1022, 1025, 1028, 1031, 1034, 1037, 1040, 1043, 1051, 1054, 1057, 1060, 1063, 1066, 1069,
    1072, 1075, 1078, 1081, 1084, 1087, 1090, 1093, 1096, 1099, 1102, 1105, 1108, 1111, 1114, 1117,
    1120, 1123, 1126, 1129, 1132, 1135, 1138, 1141, 1144, 1147, 1150, 1153, 1156, 1159, 1162, 1165,
    1168, 1171, 1174, 1177, 1180, 1183, 1186, 1189, 1192, 1195, 1198, 1201, 1204, 1207, 1210, 1213,
    1216, 1219, 1222, 1225, 1228, 1231, 1234, 1237, 1240, 1243, 1246, 1249, 1252, 1255, 1258, 1261,
    1264, 1267, 1270, 1273, 1276, 1279, 1282, 1285, 1288, 1291, 1294, 1297, 1300, 1303, 1306, 1309,
    1312, 1315, 1318, 1321, 1324, 1327, 1330, 1333, 1336, 1339, 1342, 1345, 1352, 1357, 1360, 1363,
    1366, 1369, 1372, 1375, 1378, 1381, 1387, 1401, 1415, 1421, 1427, 1433, 1439, 1445, 1451, 1457,
    1463, 1469, 1475, 1481, 1487, 1493, 1496, 1499, 1502, 1508, 1511, 1514, 1517, 1523, 1526, 1529,
    1532, 1538, 1541, 1544, 1547, 1553, 1556, 1562, 1563, 1564, 1565, 1566, 1567, 1568, 1569, 1570,
    1571, 1572, 1573, 1574, 1575, 1576, 1577, 1578, 1579, 1580, 1581, 1582, 1589, 1590, 1597, 1597,
    1597, 1597, 1601, 1601, 1605, 1605, 1609, 1609, 1613, 1613, 1617, 1617, 1617, 1617, 1617, 1617,
    1617, 1618, 1618, 1618, 1623, 1630, 1636, 1640, 1649, 1656, 1661, 1661, 1666, 1672, 1675, 1682,
    1689, 1689, 1689, 1693, 1699, 1705, 1711, 1711, 1711, 1711, 1711, 1711, 1711, 1711, 1711, 1711,
    1711, 1711, 1711, 1712, 1712, 1712, 1716, 1719, 1722, 1725, 1728, 1731, 1734, 1737, 1740, 1745,
    1745, 1745, 1745, 1745, 1745, 1745, 1745, 1745, 1745, 1745, 1745, 1745, 1746, 1746, 1750, 1757,
    1763, 1768, 1773, 1779, 1784, 1789, 1794, 1800, 1805, 1811, 1820, 1826, 1832, 1837, 1843, 1849,
    1854, 1859, 1864, 1869, 1874, 1879, 1884, 1889, 1894, 1899, 1904, 1909, 1914, 1920, 1920, 1920,
    1924, 1931, 1938, 1945, 1945, 1945, 1945, 1945, 1945, 1945, 1946, 1946, 1946, 1946, 1946, 1946,
    1946, 1946, 1947, 1947, 1947, 1947, 1947, 1947, 1947, 1951, 1961, 1964, 1970, 1973, 1980, 1989,
    1998, 1998, 1998, 1998, 1998, 1998, 1998, 1998, 1998, 1999, 1999, 1999, 1999, 1999, 1999, 2003,
    2006, 2012, 2015, 2021, 2024, 2030, 2033, 2039, 2042, 2048, 2051, 2057, 2060, 2066, 2069, 2075,
    2078, 2084, 2090, 2099, 2107, 2110, 2114, 2120, 2124, 2128, 2134, 2138, 2142, 2148, 2152, 2156,
    2162, 2166, 2170, 2176, 2180, 2184, 2190, 2194, 2198, 2204, 2208, 2212, 2218, 2222, 2226, 2232,
    2236, 2240, 2246, 2250, 2254, 2260, 2264, 2268, 2274, 2278, 2282, 2288, 2291, 2294, 2300, 2311,
    2322, 2325, 2331, 2339, 2347, 2355, 2358, 2363, 2372, 2378, 2384, 2390, 2400, 2410, 2417, 2424,
    2431, 2439, 2447, 2455, 2463, 2469, 2475, 2478, 2484, 2490, 2495, 2498, 2505, 2508, 2511, 2514,
    2517, 2520, 2523, 2526, 2531, 2533, 2543, 2545, 2551, 2570, 2573, 2580, 2583, 2589, 2603, 2604,
    2605, 2606, 2607, 2611, 2617, 2620, 2628, 2635, 2639, 2647, 2650, 2656, 2656, 2656, 2656, 2656,
    2656, 2657, 2661, 2667, 2673, 2680, 2691, 2702, 2709, 2720, 2720, 2724, 2731, 2738, 2738, 2742,
    2742, 2746, 2752, 2753, 2760, 2766, 2769, 2776, 2783, 2784, 2785, 2786, 2787, 2788, 2791, 2791,
    2791, 2791, 2791, 2791, 2791, 2793, 2798, 2803, 2808, 2813, 2818, 2823, 2829, 2830, 2831, 2832,
    2833, 2834, 2835, 2836, 2837, 2838, 2843, 2846, 2853, 2856, 2862, 2872, 2877, 2882, 2887, 2892,
    2897, 2902, 2907, 2912};

// Print the state stack on the debug stream.
void ParserGen::yystack_print_() {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

// Report on the debug stream that the rule \a yyrule is going to be reduced.
void ParserGen::yy_reduce_print_(int yyrule) {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 57 "src/mongo/db/cst/grammar.yy"
}  // namespace mongo
#line 9675 "src/mongo/db/cst/parser_gen.cpp"

#line 2916 "src/mongo/db/cst/grammar.yy"
