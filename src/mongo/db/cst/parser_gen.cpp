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
        case 144:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case 151:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case 153:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 150:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 149:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 152:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 147:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case 157:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case 146:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case 158:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case 160:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 159:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case 148:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case 145:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case 156:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case 154:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case 155:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 139:  // "fieldname containing dotted path"
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
        case 144:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case 151:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case 153:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case 150:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case 149:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case 152:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case 147:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case 157:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case 146:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case 158:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case 160:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case 159:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case 148:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case 145:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case 156:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case 154:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case 155:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case 139:  // "fieldname containing dotted path"
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
        case 144:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case 151:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case 153:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case 150:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case 149:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case 152:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case 147:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case 157:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case 146:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case 158:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case 160:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case 159:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case 148:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case 145:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case 156:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case 154:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case 155:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.copy<std::string>(that.value);
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(that.value);
            break;

        case 139:  // "fieldname containing dotted path"
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
        case 144:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case 151:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case 153:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case 150:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case 149:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case 152:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case 182:  // dbPointer
        case 183:  // javascript
        case 184:  // symbol
        case 185:  // javascriptWScope
        case 186:  // int
        case 187:  // timestamp
        case 188:  // long
        case 189:  // double
        case 190:  // decimal
        case 191:  // minKey
        case 192:  // maxKey
        case 193:  // value
        case 194:  // string
        case 195:  // aggregationFieldPath
        case 196:  // binary
        case 197:  // undefined
        case 198:  // objectId
        case 199:  // bool
        case 200:  // date
        case 201:  // null
        case 202:  // regex
        case 203:  // simpleValue
        case 204:  // compoundValue
        case 205:  // valueArray
        case 206:  // valueObject
        case 207:  // valueFields
        case 208:  // variable
        case 209:  // pipeline
        case 210:  // stageList
        case 211:  // stage
        case 212:  // inhibitOptimization
        case 213:  // unionWith
        case 214:  // skip
        case 215:  // limit
        case 216:  // project
        case 217:  // sample
        case 218:  // projectFields
        case 219:  // projectionObjectFields
        case 220:  // topLevelProjection
        case 221:  // projection
        case 222:  // projectionObject
        case 223:  // num
        case 224:  // expression
        case 225:  // compoundNonObjectExpression
        case 226:  // exprFixedTwoArg
        case 227:  // exprFixedThreeArg
        case 228:  // arrayManipulation
        case 229:  // slice
        case 230:  // expressionArray
        case 231:  // expressionObject
        case 232:  // expressionFields
        case 233:  // maths
        case 234:  // meta
        case 235:  // add
        case 236:  // boolExprs
        case 237:  // and
        case 238:  // or
        case 239:  // not
        case 240:  // literalEscapes
        case 241:  // const
        case 242:  // literal
        case 243:  // stringExps
        case 244:  // concat
        case 245:  // dateFromString
        case 246:  // dateToString
        case 247:  // indexOfBytes
        case 248:  // indexOfCP
        case 249:  // ltrim
        case 250:  // regexFind
        case 251:  // regexFindAll
        case 252:  // regexMatch
        case 253:  // regexArgs
        case 254:  // replaceOne
        case 255:  // replaceAll
        case 256:  // rtrim
        case 257:  // split
        case 258:  // strLenBytes
        case 259:  // strLenCP
        case 260:  // strcasecmp
        case 261:  // substr
        case 262:  // substrBytes
        case 263:  // substrCP
        case 264:  // toLower
        case 265:  // toUpper
        case 266:  // trim
        case 267:  // compExprs
        case 268:  // cmp
        case 269:  // eq
        case 270:  // gt
        case 271:  // gte
        case 272:  // lt
        case 273:  // lte
        case 274:  // ne
        case 275:  // typeExpression
        case 276:  // convert
        case 277:  // toBool
        case 278:  // toDate
        case 279:  // toDecimal
        case 280:  // toDouble
        case 281:  // toInt
        case 282:  // toLong
        case 283:  // toObjectId
        case 284:  // toString
        case 285:  // type
        case 286:  // abs
        case 287:  // ceil
        case 288:  // divide
        case 289:  // exponent
        case 290:  // floor
        case 291:  // ln
        case 292:  // log
        case 293:  // logten
        case 294:  // mod
        case 295:  // multiply
        case 296:  // pow
        case 297:  // round
        case 298:  // sqrt
        case 299:  // subtract
        case 300:  // trunc
        case 310:  // setExpression
        case 311:  // allElementsTrue
        case 312:  // anyElementTrue
        case 313:  // setDifference
        case 314:  // setEquals
        case 315:  // setIntersection
        case 316:  // setIsSubset
        case 317:  // setUnion
        case 318:  // trig
        case 319:  // sin
        case 320:  // cos
        case 321:  // tan
        case 322:  // sinh
        case 323:  // cosh
        case 324:  // tanh
        case 325:  // asin
        case 326:  // acos
        case 327:  // atan
        case 328:  // asinh
        case 329:  // acosh
        case 330:  // atanh
        case 331:  // atan2
        case 332:  // degreesToRadians
        case 333:  // radiansToDegrees
        case 334:  // nonArrayExpression
        case 335:  // nonArrayCompoundExpression
        case 336:  // nonArrayNonObjCompoundExpression
        case 337:  // expressionSingletonArray
        case 338:  // singleArgExpression
        case 339:  // match
        case 340:  // predicates
        case 341:  // compoundMatchExprs
        case 342:  // predValue
        case 343:  // additionalExprs
        case 349:  // sortSpecs
        case 350:  // specList
        case 351:  // metaSort
        case 352:  // oneOrNegOne
        case 353:  // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case 165:  // aggregationProjectionFieldname
        case 166:  // projectionFieldname
        case 167:  // expressionFieldname
        case 168:  // stageAsUserFieldname
        case 169:  // argAsUserFieldname
        case 170:  // argAsProjectionPath
        case 171:  // aggExprAsUserFieldname
        case 172:  // invariableUserFieldname
        case 173:  // idAsUserFieldname
        case 174:  // idAsProjectionPath
        case 175:  // valueFieldname
        case 176:  // predFieldname
        case 348:  // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case 147:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case 157:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case 146:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case 158:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case 160:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case 159:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case 148:  // "null"
            value.move<UserNull>(that.value);
            break;

        case 145:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case 156:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case 154:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case 155:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case 177:  // projectField
        case 178:  // projectionObjectField
        case 179:  // expressionField
        case 180:  // valueField
        case 301:  // onErrorArg
        case 302:  // onNullArg
        case 303:  // formatArg
        case 304:  // timezoneArg
        case 305:  // charsArg
        case 306:  // optionsArg
        case 344:  // predicate
        case 345:  // logicalExpr
        case 346:  // operatorExpression
        case 347:  // notExpr
        case 354:  // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case 138:  // "fieldname"
        case 140:  // "$-prefixed fieldname"
        case 141:  // "string"
        case 142:  // "$-prefixed string"
        case 143:  // "$$-prefixed string"
        case 181:  // arg
            value.move<std::string>(that.value);
            break;

        case 307:  // expressions
        case 308:  // values
        case 309:  // exprZeroToTwo
            value.move<std::vector<CNode>>(that.value);
            break;

        case 139:  // "fieldname containing dotted path"
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
                case 144:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case 151:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case 153:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case 150:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case 149:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case 152:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case 182:  // dbPointer
                case 183:  // javascript
                case 184:  // symbol
                case 185:  // javascriptWScope
                case 186:  // int
                case 187:  // timestamp
                case 188:  // long
                case 189:  // double
                case 190:  // decimal
                case 191:  // minKey
                case 192:  // maxKey
                case 193:  // value
                case 194:  // string
                case 195:  // aggregationFieldPath
                case 196:  // binary
                case 197:  // undefined
                case 198:  // objectId
                case 199:  // bool
                case 200:  // date
                case 201:  // null
                case 202:  // regex
                case 203:  // simpleValue
                case 204:  // compoundValue
                case 205:  // valueArray
                case 206:  // valueObject
                case 207:  // valueFields
                case 208:  // variable
                case 209:  // pipeline
                case 210:  // stageList
                case 211:  // stage
                case 212:  // inhibitOptimization
                case 213:  // unionWith
                case 214:  // skip
                case 215:  // limit
                case 216:  // project
                case 217:  // sample
                case 218:  // projectFields
                case 219:  // projectionObjectFields
                case 220:  // topLevelProjection
                case 221:  // projection
                case 222:  // projectionObject
                case 223:  // num
                case 224:  // expression
                case 225:  // compoundNonObjectExpression
                case 226:  // exprFixedTwoArg
                case 227:  // exprFixedThreeArg
                case 228:  // arrayManipulation
                case 229:  // slice
                case 230:  // expressionArray
                case 231:  // expressionObject
                case 232:  // expressionFields
                case 233:  // maths
                case 234:  // meta
                case 235:  // add
                case 236:  // boolExprs
                case 237:  // and
                case 238:  // or
                case 239:  // not
                case 240:  // literalEscapes
                case 241:  // const
                case 242:  // literal
                case 243:  // stringExps
                case 244:  // concat
                case 245:  // dateFromString
                case 246:  // dateToString
                case 247:  // indexOfBytes
                case 248:  // indexOfCP
                case 249:  // ltrim
                case 250:  // regexFind
                case 251:  // regexFindAll
                case 252:  // regexMatch
                case 253:  // regexArgs
                case 254:  // replaceOne
                case 255:  // replaceAll
                case 256:  // rtrim
                case 257:  // split
                case 258:  // strLenBytes
                case 259:  // strLenCP
                case 260:  // strcasecmp
                case 261:  // substr
                case 262:  // substrBytes
                case 263:  // substrCP
                case 264:  // toLower
                case 265:  // toUpper
                case 266:  // trim
                case 267:  // compExprs
                case 268:  // cmp
                case 269:  // eq
                case 270:  // gt
                case 271:  // gte
                case 272:  // lt
                case 273:  // lte
                case 274:  // ne
                case 275:  // typeExpression
                case 276:  // convert
                case 277:  // toBool
                case 278:  // toDate
                case 279:  // toDecimal
                case 280:  // toDouble
                case 281:  // toInt
                case 282:  // toLong
                case 283:  // toObjectId
                case 284:  // toString
                case 285:  // type
                case 286:  // abs
                case 287:  // ceil
                case 288:  // divide
                case 289:  // exponent
                case 290:  // floor
                case 291:  // ln
                case 292:  // log
                case 293:  // logten
                case 294:  // mod
                case 295:  // multiply
                case 296:  // pow
                case 297:  // round
                case 298:  // sqrt
                case 299:  // subtract
                case 300:  // trunc
                case 310:  // setExpression
                case 311:  // allElementsTrue
                case 312:  // anyElementTrue
                case 313:  // setDifference
                case 314:  // setEquals
                case 315:  // setIntersection
                case 316:  // setIsSubset
                case 317:  // setUnion
                case 318:  // trig
                case 319:  // sin
                case 320:  // cos
                case 321:  // tan
                case 322:  // sinh
                case 323:  // cosh
                case 324:  // tanh
                case 325:  // asin
                case 326:  // acos
                case 327:  // atan
                case 328:  // asinh
                case 329:  // acosh
                case 330:  // atanh
                case 331:  // atan2
                case 332:  // degreesToRadians
                case 333:  // radiansToDegrees
                case 334:  // nonArrayExpression
                case 335:  // nonArrayCompoundExpression
                case 336:  // nonArrayNonObjCompoundExpression
                case 337:  // expressionSingletonArray
                case 338:  // singleArgExpression
                case 339:  // match
                case 340:  // predicates
                case 341:  // compoundMatchExprs
                case 342:  // predValue
                case 343:  // additionalExprs
                case 349:  // sortSpecs
                case 350:  // specList
                case 351:  // metaSort
                case 352:  // oneOrNegOne
                case 353:  // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case 165:  // aggregationProjectionFieldname
                case 166:  // projectionFieldname
                case 167:  // expressionFieldname
                case 168:  // stageAsUserFieldname
                case 169:  // argAsUserFieldname
                case 170:  // argAsProjectionPath
                case 171:  // aggExprAsUserFieldname
                case 172:  // invariableUserFieldname
                case 173:  // idAsUserFieldname
                case 174:  // idAsProjectionPath
                case 175:  // valueFieldname
                case 176:  // predFieldname
                case 348:  // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case 147:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case 157:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case 146:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case 158:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case 160:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case 159:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case 148:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case 145:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case 156:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case 154:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case 155:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case 177:  // projectField
                case 178:  // projectionObjectField
                case 179:  // expressionField
                case 180:  // valueField
                case 301:  // onErrorArg
                case 302:  // onNullArg
                case 303:  // formatArg
                case 304:  // timezoneArg
                case 305:  // charsArg
                case 306:  // optionsArg
                case 344:  // predicate
                case 345:  // logicalExpr
                case 346:  // operatorExpression
                case 347:  // notExpr
                case 354:  // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case 138:  // "fieldname"
                case 140:  // "$-prefixed fieldname"
                case 141:  // "string"
                case 142:  // "$-prefixed string"
                case 143:  // "$$-prefixed string"
                case 181:  // arg
                    yylhs.value.emplace<std::string>();
                    break;

                case 307:  // expressions
                case 308:  // values
                case 309:  // exprZeroToTwo
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                case 139:  // "fieldname containing dotted path"
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
#line 349 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1973 "parser_gen.cpp"
                    break;

                    case 3:
#line 352 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1981 "parser_gen.cpp"
                    break;

                    case 4:
#line 355 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1989 "parser_gen.cpp"
                    break;

                    case 5:
#line 362 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1997 "parser_gen.cpp"
                    break;

                    case 6:
#line 368 "grammar.yy"
                    {
                    }
#line 2003 "parser_gen.cpp"
                    break;

                    case 7:
#line 369 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2011 "parser_gen.cpp"
                    break;

                    case 8:
#line 377 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2017 "parser_gen.cpp"
                    break;

                    case 10:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2023 "parser_gen.cpp"
                    break;

                    case 11:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2029 "parser_gen.cpp"
                    break;

                    case 12:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2035 "parser_gen.cpp"
                    break;

                    case 13:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2041 "parser_gen.cpp"
                    break;

                    case 14:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2047 "parser_gen.cpp"
                    break;

                    case 15:
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2053 "parser_gen.cpp"
                    break;

                    case 16:
#line 383 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2065 "parser_gen.cpp"
                    break;

                    case 17:
#line 393 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2073 "parser_gen.cpp"
                    break;

                    case 18:
#line 399 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2086 "parser_gen.cpp"
                    break;

                    case 19:
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2092 "parser_gen.cpp"
                    break;

                    case 20:
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2098 "parser_gen.cpp"
                    break;

                    case 21:
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2104 "parser_gen.cpp"
                    break;

                    case 22:
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2110 "parser_gen.cpp"
                    break;

                    case 23:
#line 413 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2118 "parser_gen.cpp"
                    break;

                    case 24:
#line 418 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2126 "parser_gen.cpp"
                    break;

                    case 25:
#line 423 "grammar.yy"
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
#line 2147 "parser_gen.cpp"
                    break;

                    case 26:
#line 442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2155 "parser_gen.cpp"
                    break;

                    case 27:
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2164 "parser_gen.cpp"
                    break;

                    case 28:
#line 452 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2172 "parser_gen.cpp"
                    break;

                    case 29:
#line 455 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2180 "parser_gen.cpp"
                    break;

                    case 30:
#line 461 "grammar.yy"
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
#line 2196 "parser_gen.cpp"
                    break;

                    case 31:
#line 475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2202 "parser_gen.cpp"
                    break;

                    case 32:
#line 476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2208 "parser_gen.cpp"
                    break;

                    case 33:
#line 477 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2214 "parser_gen.cpp"
                    break;

                    case 34:
#line 478 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2220 "parser_gen.cpp"
                    break;

                    case 35:
#line 479 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2226 "parser_gen.cpp"
                    break;

                    case 36:
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2232 "parser_gen.cpp"
                    break;

                    case 37:
#line 481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2238 "parser_gen.cpp"
                    break;

                    case 38:
#line 482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2244 "parser_gen.cpp"
                    break;

                    case 39:
#line 483 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2250 "parser_gen.cpp"
                    break;

                    case 40:
#line 484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2256 "parser_gen.cpp"
                    break;

                    case 41:
#line 485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2262 "parser_gen.cpp"
                    break;

                    case 42:
#line 486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2270 "parser_gen.cpp"
                    break;

                    case 43:
#line 489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2278 "parser_gen.cpp"
                    break;

                    case 44:
#line 492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2286 "parser_gen.cpp"
                    break;

                    case 45:
#line 495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2294 "parser_gen.cpp"
                    break;

                    case 46:
#line 498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2302 "parser_gen.cpp"
                    break;

                    case 47:
#line 501 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2310 "parser_gen.cpp"
                    break;

                    case 48:
#line 504 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2318 "parser_gen.cpp"
                    break;

                    case 49:
#line 507 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2326 "parser_gen.cpp"
                    break;

                    case 50:
#line 510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2334 "parser_gen.cpp"
                    break;

                    case 51:
#line 513 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2342 "parser_gen.cpp"
                    break;

                    case 52:
#line 516 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2350 "parser_gen.cpp"
                    break;

                    case 53:
#line 519 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2358 "parser_gen.cpp"
                    break;

                    case 54:
#line 522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2366 "parser_gen.cpp"
                    break;

                    case 55:
#line 525 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2374 "parser_gen.cpp"
                    break;

                    case 56:
#line 528 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2382 "parser_gen.cpp"
                    break;

                    case 57:
#line 531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2390 "parser_gen.cpp"
                    break;

                    case 58:
#line 534 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2398 "parser_gen.cpp"
                    break;

                    case 59:
#line 537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2406 "parser_gen.cpp"
                    break;

                    case 60:
#line 540 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2412 "parser_gen.cpp"
                    break;

                    case 61:
#line 541 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2418 "parser_gen.cpp"
                    break;

                    case 62:
#line 542 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2424 "parser_gen.cpp"
                    break;

                    case 63:
#line 543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2430 "parser_gen.cpp"
                    break;

                    case 64:
#line 544 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2436 "parser_gen.cpp"
                    break;

                    case 65:
#line 549 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2446 "parser_gen.cpp"
                    break;

                    case 66:
#line 557 "grammar.yy"
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
#line 2464 "parser_gen.cpp"
                    break;

                    case 67:
#line 570 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2470 "parser_gen.cpp"
                    break;

                    case 68:
#line 571 "grammar.yy"
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
#line 2488 "parser_gen.cpp"
                    break;

                    case 69:
#line 588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2496 "parser_gen.cpp"
                    break;

                    case 70:
#line 595 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2505 "parser_gen.cpp"
                    break;

                    case 71:
#line 599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2514 "parser_gen.cpp"
                    break;

                    case 72:
#line 607 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2522 "parser_gen.cpp"
                    break;

                    case 73:
#line 610 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2530 "parser_gen.cpp"
                    break;

                    case 74:
#line 616 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2538 "parser_gen.cpp"
                    break;

                    case 75:
#line 622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2546 "parser_gen.cpp"
                    break;

                    case 76:
#line 625 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2555 "parser_gen.cpp"
                    break;

                    case 77:
#line 631 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2563 "parser_gen.cpp"
                    break;

                    case 78:
#line 634 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2571 "parser_gen.cpp"
                    break;

                    case 79:
#line 643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2577 "parser_gen.cpp"
                    break;

                    case 80:
#line 644 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2585 "parser_gen.cpp"
                    break;

                    case 81:
#line 650 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2593 "parser_gen.cpp"
                    break;

                    case 82:
#line 653 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2602 "parser_gen.cpp"
                    break;

                    case 83:
#line 660 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2608 "parser_gen.cpp"
                    break;

                    case 84:
#line 663 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2616 "parser_gen.cpp"
                    break;

                    case 85:
#line 668 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2627 "parser_gen.cpp"
                    break;

                    case 86:
#line 678 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2637 "parser_gen.cpp"
                    break;

                    case 87:
#line 686 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2643 "parser_gen.cpp"
                    break;

                    case 88:
#line 687 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2649 "parser_gen.cpp"
                    break;

                    case 89:
#line 688 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2655 "parser_gen.cpp"
                    break;

                    case 90:
#line 691 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2663 "parser_gen.cpp"
                    break;

                    case 91:
#line 694 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2672 "parser_gen.cpp"
                    break;

                    case 92:
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2678 "parser_gen.cpp"
                    break;

                    case 93:
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2684 "parser_gen.cpp"
                    break;

                    case 94:
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2690 "parser_gen.cpp"
                    break;

                    case 95:
#line 704 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2698 "parser_gen.cpp"
                    break;

                    case 96:
#line 712 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2706 "parser_gen.cpp"
                    break;

                    case 97:
#line 715 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2714 "parser_gen.cpp"
                    break;

                    case 98:
#line 718 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2722 "parser_gen.cpp"
                    break;

                    case 99:
#line 721 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2730 "parser_gen.cpp"
                    break;

                    case 100:
#line 724 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2738 "parser_gen.cpp"
                    break;

                    case 101:
#line 727 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2746 "parser_gen.cpp"
                    break;

                    case 102:
#line 733 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2754 "parser_gen.cpp"
                    break;

                    case 103:
#line 739 "grammar.yy"
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
#line 2772 "parser_gen.cpp"
                    break;

                    case 104:
#line 758 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 2780 "parser_gen.cpp"
                    break;

                    case 105:
#line 761 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 2788 "parser_gen.cpp"
                    break;

                    case 106:
#line 764 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 2796 "parser_gen.cpp"
                    break;

                    case 107:
#line 767 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 2804 "parser_gen.cpp"
                    break;

                    case 108:
#line 770 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 2812 "parser_gen.cpp"
                    break;

                    case 109:
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 2820 "parser_gen.cpp"
                    break;

                    case 110:
#line 776 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 2828 "parser_gen.cpp"
                    break;

                    case 111:
#line 779 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 2836 "parser_gen.cpp"
                    break;

                    case 112:
#line 782 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 2844 "parser_gen.cpp"
                    break;

                    case 113:
#line 785 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 2852 "parser_gen.cpp"
                    break;

                    case 114:
#line 788 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 2860 "parser_gen.cpp"
                    break;

                    case 115:
#line 791 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 2868 "parser_gen.cpp"
                    break;

                    case 116:
#line 794 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 2876 "parser_gen.cpp"
                    break;

                    case 117:
#line 797 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 2884 "parser_gen.cpp"
                    break;

                    case 118:
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 2892 "parser_gen.cpp"
                    break;

                    case 119:
#line 803 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 2900 "parser_gen.cpp"
                    break;

                    case 120:
#line 811 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2908 "parser_gen.cpp"
                    break;

                    case 121:
#line 814 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2916 "parser_gen.cpp"
                    break;

                    case 122:
#line 817 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2924 "parser_gen.cpp"
                    break;

                    case 123:
#line 820 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2932 "parser_gen.cpp"
                    break;

                    case 124:
#line 823 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2940 "parser_gen.cpp"
                    break;

                    case 125:
#line 826 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2948 "parser_gen.cpp"
                    break;

                    case 126:
#line 829 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2956 "parser_gen.cpp"
                    break;

                    case 127:
#line 832 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2964 "parser_gen.cpp"
                    break;

                    case 128:
#line 835 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2972 "parser_gen.cpp"
                    break;

                    case 129:
#line 838 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2980 "parser_gen.cpp"
                    break;

                    case 130:
#line 841 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2988 "parser_gen.cpp"
                    break;

                    case 131:
#line 844 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2996 "parser_gen.cpp"
                    break;

                    case 132:
#line 847 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3004 "parser_gen.cpp"
                    break;

                    case 133:
#line 850 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3012 "parser_gen.cpp"
                    break;

                    case 134:
#line 853 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3020 "parser_gen.cpp"
                    break;

                    case 135:
#line 856 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3028 "parser_gen.cpp"
                    break;

                    case 136:
#line 859 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3036 "parser_gen.cpp"
                    break;

                    case 137:
#line 862 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3044 "parser_gen.cpp"
                    break;

                    case 138:
#line 865 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3052 "parser_gen.cpp"
                    break;

                    case 139:
#line 868 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3060 "parser_gen.cpp"
                    break;

                    case 140:
#line 871 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3068 "parser_gen.cpp"
                    break;

                    case 141:
#line 874 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3076 "parser_gen.cpp"
                    break;

                    case 142:
#line 877 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3084 "parser_gen.cpp"
                    break;

                    case 143:
#line 880 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3092 "parser_gen.cpp"
                    break;

                    case 144:
#line 883 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3100 "parser_gen.cpp"
                    break;

                    case 145:
#line 886 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3108 "parser_gen.cpp"
                    break;

                    case 146:
#line 889 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3116 "parser_gen.cpp"
                    break;

                    case 147:
#line 892 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3124 "parser_gen.cpp"
                    break;

                    case 148:
#line 895 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3132 "parser_gen.cpp"
                    break;

                    case 149:
#line 898 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3140 "parser_gen.cpp"
                    break;

                    case 150:
#line 901 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3148 "parser_gen.cpp"
                    break;

                    case 151:
#line 904 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3156 "parser_gen.cpp"
                    break;

                    case 152:
#line 907 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3164 "parser_gen.cpp"
                    break;

                    case 153:
#line 910 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3172 "parser_gen.cpp"
                    break;

                    case 154:
#line 913 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3180 "parser_gen.cpp"
                    break;

                    case 155:
#line 916 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3188 "parser_gen.cpp"
                    break;

                    case 156:
#line 919 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3196 "parser_gen.cpp"
                    break;

                    case 157:
#line 922 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3204 "parser_gen.cpp"
                    break;

                    case 158:
#line 925 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3212 "parser_gen.cpp"
                    break;

                    case 159:
#line 928 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3220 "parser_gen.cpp"
                    break;

                    case 160:
#line 931 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3228 "parser_gen.cpp"
                    break;

                    case 161:
#line 934 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3236 "parser_gen.cpp"
                    break;

                    case 162:
#line 937 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3244 "parser_gen.cpp"
                    break;

                    case 163:
#line 940 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3252 "parser_gen.cpp"
                    break;

                    case 164:
#line 943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3260 "parser_gen.cpp"
                    break;

                    case 165:
#line 946 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3268 "parser_gen.cpp"
                    break;

                    case 166:
#line 949 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3276 "parser_gen.cpp"
                    break;

                    case 167:
#line 952 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3284 "parser_gen.cpp"
                    break;

                    case 168:
#line 955 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3292 "parser_gen.cpp"
                    break;

                    case 169:
#line 958 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3300 "parser_gen.cpp"
                    break;

                    case 170:
#line 961 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3308 "parser_gen.cpp"
                    break;

                    case 171:
#line 964 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3316 "parser_gen.cpp"
                    break;

                    case 172:
#line 967 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3324 "parser_gen.cpp"
                    break;

                    case 173:
#line 970 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3332 "parser_gen.cpp"
                    break;

                    case 174:
#line 973 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3340 "parser_gen.cpp"
                    break;

                    case 175:
#line 976 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3348 "parser_gen.cpp"
                    break;

                    case 176:
#line 979 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3356 "parser_gen.cpp"
                    break;

                    case 177:
#line 982 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3364 "parser_gen.cpp"
                    break;

                    case 178:
#line 985 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3372 "parser_gen.cpp"
                    break;

                    case 179:
#line 988 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3380 "parser_gen.cpp"
                    break;

                    case 180:
#line 991 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3388 "parser_gen.cpp"
                    break;

                    case 181:
#line 994 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3396 "parser_gen.cpp"
                    break;

                    case 182:
#line 997 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3404 "parser_gen.cpp"
                    break;

                    case 183:
#line 1000 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3412 "parser_gen.cpp"
                    break;

                    case 184:
#line 1003 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3420 "parser_gen.cpp"
                    break;

                    case 185:
#line 1006 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3428 "parser_gen.cpp"
                    break;

                    case 186:
#line 1009 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3436 "parser_gen.cpp"
                    break;

                    case 187:
#line 1012 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3444 "parser_gen.cpp"
                    break;

                    case 188:
#line 1015 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3452 "parser_gen.cpp"
                    break;

                    case 189:
#line 1018 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3460 "parser_gen.cpp"
                    break;

                    case 190:
#line 1021 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 3468 "parser_gen.cpp"
                    break;

                    case 191:
#line 1024 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 3476 "parser_gen.cpp"
                    break;

                    case 192:
#line 1027 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 3484 "parser_gen.cpp"
                    break;

                    case 193:
#line 1030 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 3492 "parser_gen.cpp"
                    break;

                    case 194:
#line 1033 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 3500 "parser_gen.cpp"
                    break;

                    case 195:
#line 1036 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 3508 "parser_gen.cpp"
                    break;

                    case 196:
#line 1039 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 3516 "parser_gen.cpp"
                    break;

                    case 197:
#line 1042 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 3524 "parser_gen.cpp"
                    break;

                    case 198:
#line 1045 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 3532 "parser_gen.cpp"
                    break;

                    case 199:
#line 1048 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 3540 "parser_gen.cpp"
                    break;

                    case 200:
#line 1051 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 3548 "parser_gen.cpp"
                    break;

                    case 201:
#line 1054 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 3556 "parser_gen.cpp"
                    break;

                    case 202:
#line 1057 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 3564 "parser_gen.cpp"
                    break;

                    case 203:
#line 1060 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 3572 "parser_gen.cpp"
                    break;

                    case 204:
#line 1067 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3580 "parser_gen.cpp"
                    break;

                    case 205:
#line 1072 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 3588 "parser_gen.cpp"
                    break;

                    case 206:
#line 1075 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 3596 "parser_gen.cpp"
                    break;

                    case 207:
#line 1078 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 3604 "parser_gen.cpp"
                    break;

                    case 208:
#line 1081 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3612 "parser_gen.cpp"
                    break;

                    case 209:
#line 1084 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 3620 "parser_gen.cpp"
                    break;

                    case 210:
#line 1087 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 3628 "parser_gen.cpp"
                    break;

                    case 211:
#line 1090 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 3636 "parser_gen.cpp"
                    break;

                    case 212:
#line 1093 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 3644 "parser_gen.cpp"
                    break;

                    case 213:
#line 1096 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3652 "parser_gen.cpp"
                    break;

                    case 214:
#line 1102 "grammar.yy"
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
#line 3668 "parser_gen.cpp"
                    break;

                    case 215:
#line 1116 "grammar.yy"
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
#line 3684 "parser_gen.cpp"
                    break;

                    case 216:
#line 1130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3692 "parser_gen.cpp"
                    break;

                    case 217:
#line 1136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3700 "parser_gen.cpp"
                    break;

                    case 218:
#line 1142 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3708 "parser_gen.cpp"
                    break;

                    case 219:
#line 1148 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3716 "parser_gen.cpp"
                    break;

                    case 220:
#line 1154 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3724 "parser_gen.cpp"
                    break;

                    case 221:
#line 1160 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3732 "parser_gen.cpp"
                    break;

                    case 222:
#line 1166 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3740 "parser_gen.cpp"
                    break;

                    case 223:
#line 1172 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3748 "parser_gen.cpp"
                    break;

                    case 224:
#line 1178 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3756 "parser_gen.cpp"
                    break;

                    case 225:
#line 1184 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3764 "parser_gen.cpp"
                    break;

                    case 226:
#line 1190 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3772 "parser_gen.cpp"
                    break;

                    case 227:
#line 1196 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3780 "parser_gen.cpp"
                    break;

                    case 228:
#line 1202 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3788 "parser_gen.cpp"
                    break;

                    case 229:
#line 1208 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3796 "parser_gen.cpp"
                    break;

                    case 230:
#line 1211 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3804 "parser_gen.cpp"
                    break;

                    case 231:
#line 1214 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3812 "parser_gen.cpp"
                    break;

                    case 232:
#line 1217 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3820 "parser_gen.cpp"
                    break;

                    case 233:
#line 1223 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3828 "parser_gen.cpp"
                    break;

                    case 234:
#line 1226 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3836 "parser_gen.cpp"
                    break;

                    case 235:
#line 1229 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3844 "parser_gen.cpp"
                    break;

                    case 236:
#line 1232 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3852 "parser_gen.cpp"
                    break;

                    case 237:
#line 1238 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3860 "parser_gen.cpp"
                    break;

                    case 238:
#line 1241 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3868 "parser_gen.cpp"
                    break;

                    case 239:
#line 1244 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3876 "parser_gen.cpp"
                    break;

                    case 240:
#line 1247 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3884 "parser_gen.cpp"
                    break;

                    case 241:
#line 1253 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3892 "parser_gen.cpp"
                    break;

                    case 242:
#line 1256 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3900 "parser_gen.cpp"
                    break;

                    case 243:
#line 1259 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3908 "parser_gen.cpp"
                    break;

                    case 244:
#line 1262 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3916 "parser_gen.cpp"
                    break;

                    case 245:
#line 1268 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3924 "parser_gen.cpp"
                    break;

                    case 246:
#line 1271 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3932 "parser_gen.cpp"
                    break;

                    case 247:
#line 1277 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3938 "parser_gen.cpp"
                    break;

                    case 248:
#line 1278 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3944 "parser_gen.cpp"
                    break;

                    case 249:
#line 1279 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3950 "parser_gen.cpp"
                    break;

                    case 250:
#line 1280 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3956 "parser_gen.cpp"
                    break;

                    case 251:
#line 1281 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3962 "parser_gen.cpp"
                    break;

                    case 252:
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3968 "parser_gen.cpp"
                    break;

                    case 253:
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3974 "parser_gen.cpp"
                    break;

                    case 254:
#line 1284 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3980 "parser_gen.cpp"
                    break;

                    case 255:
#line 1285 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3986 "parser_gen.cpp"
                    break;

                    case 256:
#line 1286 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3992 "parser_gen.cpp"
                    break;

                    case 257:
#line 1287 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3998 "parser_gen.cpp"
                    break;

                    case 258:
#line 1288 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4004 "parser_gen.cpp"
                    break;

                    case 259:
#line 1289 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4010 "parser_gen.cpp"
                    break;

                    case 260:
#line 1290 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4016 "parser_gen.cpp"
                    break;

                    case 261:
#line 1291 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4022 "parser_gen.cpp"
                    break;

                    case 262:
#line 1292 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4028 "parser_gen.cpp"
                    break;

                    case 263:
#line 1293 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4034 "parser_gen.cpp"
                    break;

                    case 264:
#line 1294 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4040 "parser_gen.cpp"
                    break;

                    case 265:
#line 1295 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4046 "parser_gen.cpp"
                    break;

                    case 266:
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4052 "parser_gen.cpp"
                    break;

                    case 267:
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4058 "parser_gen.cpp"
                    break;

                    case 268:
#line 1304 "grammar.yy"
                    {
                    }
#line 4064 "parser_gen.cpp"
                    break;

                    case 269:
#line 1305 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4073 "parser_gen.cpp"
                    break;

                    case 270:
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4079 "parser_gen.cpp"
                    break;

                    case 271:
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4085 "parser_gen.cpp"
                    break;

                    case 272:
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4091 "parser_gen.cpp"
                    break;

                    case 273:
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4097 "parser_gen.cpp"
                    break;

                    case 274:
#line 1316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4103 "parser_gen.cpp"
                    break;

                    case 275:
#line 1316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4109 "parser_gen.cpp"
                    break;

                    case 276:
#line 1320 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4115 "parser_gen.cpp"
                    break;

                    case 277:
#line 1320 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4121 "parser_gen.cpp"
                    break;

                    case 278:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4127 "parser_gen.cpp"
                    break;

                    case 279:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4133 "parser_gen.cpp"
                    break;

                    case 280:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4139 "parser_gen.cpp"
                    break;

                    case 281:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4145 "parser_gen.cpp"
                    break;

                    case 282:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4151 "parser_gen.cpp"
                    break;

                    case 283:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4157 "parser_gen.cpp"
                    break;

                    case 284:
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4163 "parser_gen.cpp"
                    break;

                    case 285:
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4169 "parser_gen.cpp"
                    break;

                    case 286:
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4175 "parser_gen.cpp"
                    break;

                    case 287:
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4181 "parser_gen.cpp"
                    break;

                    case 288:
#line 1330 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4189 "parser_gen.cpp"
                    break;

                    case 289:
#line 1337 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4197 "parser_gen.cpp"
                    break;

                    case 290:
#line 1343 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4203 "parser_gen.cpp"
                    break;

                    case 291:
#line 1343 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4209 "parser_gen.cpp"
                    break;

                    case 292:
#line 1347 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4215 "parser_gen.cpp"
                    break;

                    case 293:
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4224 "parser_gen.cpp"
                    break;

                    case 294:
#line 1355 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4233 "parser_gen.cpp"
                    break;

                    case 295:
#line 1364 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4241 "parser_gen.cpp"
                    break;

                    case 296:
#line 1371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4249 "parser_gen.cpp"
                    break;

                    case 297:
#line 1376 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4255 "parser_gen.cpp"
                    break;

                    case 298:
#line 1376 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4261 "parser_gen.cpp"
                    break;

                    case 299:
#line 1381 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4269 "parser_gen.cpp"
                    break;

                    case 300:
#line 1387 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4277 "parser_gen.cpp"
                    break;

                    case 301:
#line 1390 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4286 "parser_gen.cpp"
                    break;

                    case 302:
#line 1397 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4294 "parser_gen.cpp"
                    break;

                    case 303:
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4300 "parser_gen.cpp"
                    break;

                    case 304:
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4306 "parser_gen.cpp"
                    break;

                    case 305:
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4312 "parser_gen.cpp"
                    break;

                    case 306:
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4318 "parser_gen.cpp"
                    break;

                    case 307:
#line 1408 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4326 "parser_gen.cpp"
                    break;

                    case 308:
#line 1414 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{make_vector<std::string>("_id")};
                    }
#line 4334 "parser_gen.cpp"
                    break;

                    case 309:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4340 "parser_gen.cpp"
                    break;

                    case 310:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4346 "parser_gen.cpp"
                    break;

                    case 311:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4352 "parser_gen.cpp"
                    break;

                    case 312:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4358 "parser_gen.cpp"
                    break;

                    case 313:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4364 "parser_gen.cpp"
                    break;

                    case 314:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4370 "parser_gen.cpp"
                    break;

                    case 315:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4376 "parser_gen.cpp"
                    break;

                    case 316:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4382 "parser_gen.cpp"
                    break;

                    case 317:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4388 "parser_gen.cpp"
                    break;

                    case 318:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4394 "parser_gen.cpp"
                    break;

                    case 319:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4400 "parser_gen.cpp"
                    break;

                    case 320:
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4406 "parser_gen.cpp"
                    break;

                    case 321:
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4412 "parser_gen.cpp"
                    break;

                    case 322:
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4418 "parser_gen.cpp"
                    break;

                    case 323:
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4424 "parser_gen.cpp"
                    break;

                    case 324:
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4430 "parser_gen.cpp"
                    break;

                    case 325:
#line 1425 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4438 "parser_gen.cpp"
                    break;

                    case 326:
#line 1428 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4446 "parser_gen.cpp"
                    break;

                    case 327:
#line 1431 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4454 "parser_gen.cpp"
                    break;

                    case 328:
#line 1434 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4462 "parser_gen.cpp"
                    break;

                    case 329:
#line 1437 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4470 "parser_gen.cpp"
                    break;

                    case 330:
#line 1440 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4478 "parser_gen.cpp"
                    break;

                    case 331:
#line 1443 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4486 "parser_gen.cpp"
                    break;

                    case 332:
#line 1446 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4494 "parser_gen.cpp"
                    break;

                    case 333:
#line 1449 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4502 "parser_gen.cpp"
                    break;

                    case 334:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4508 "parser_gen.cpp"
                    break;

                    case 335:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4514 "parser_gen.cpp"
                    break;

                    case 336:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4520 "parser_gen.cpp"
                    break;

                    case 337:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4526 "parser_gen.cpp"
                    break;

                    case 338:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4532 "parser_gen.cpp"
                    break;

                    case 339:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4538 "parser_gen.cpp"
                    break;

                    case 340:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4544 "parser_gen.cpp"
                    break;

                    case 341:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4550 "parser_gen.cpp"
                    break;

                    case 342:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4556 "parser_gen.cpp"
                    break;

                    case 343:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4562 "parser_gen.cpp"
                    break;

                    case 344:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4568 "parser_gen.cpp"
                    break;

                    case 345:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4574 "parser_gen.cpp"
                    break;

                    case 346:
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4580 "parser_gen.cpp"
                    break;

                    case 347:
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4586 "parser_gen.cpp"
                    break;

                    case 348:
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4592 "parser_gen.cpp"
                    break;

                    case 349:
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4601 "parser_gen.cpp"
                    break;

                    case 350:
#line 1466 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4610 "parser_gen.cpp"
                    break;

                    case 351:
#line 1472 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4618 "parser_gen.cpp"
                    break;

                    case 352:
#line 1477 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4626 "parser_gen.cpp"
                    break;

                    case 353:
#line 1482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4635 "parser_gen.cpp"
                    break;

                    case 354:
#line 1488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4643 "parser_gen.cpp"
                    break;

                    case 355:
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4651 "parser_gen.cpp"
                    break;

                    case 356:
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4659 "parser_gen.cpp"
                    break;

                    case 357:
#line 1503 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4668 "parser_gen.cpp"
                    break;

                    case 358:
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4676 "parser_gen.cpp"
                    break;

                    case 359:
#line 1514 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4685 "parser_gen.cpp"
                    break;

                    case 360:
#line 1520 "grammar.yy"
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
#line 4697 "parser_gen.cpp"
                    break;

                    case 361:
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4706 "parser_gen.cpp"
                    break;

                    case 362:
#line 1535 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4715 "parser_gen.cpp"
                    break;

                    case 363:
#line 1541 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4723 "parser_gen.cpp"
                    break;

                    case 364:
#line 1546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4732 "parser_gen.cpp"
                    break;

                    case 365:
#line 1552 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4741 "parser_gen.cpp"
                    break;

                    case 366:
#line 1558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4749 "parser_gen.cpp"
                    break;

                    case 367:
#line 1563 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4757 "parser_gen.cpp"
                    break;

                    case 368:
#line 1568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4765 "parser_gen.cpp"
                    break;

                    case 369:
#line 1573 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4773 "parser_gen.cpp"
                    break;

                    case 370:
#line 1578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4781 "parser_gen.cpp"
                    break;

                    case 371:
#line 1583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4789 "parser_gen.cpp"
                    break;

                    case 372:
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4797 "parser_gen.cpp"
                    break;

                    case 373:
#line 1593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4805 "parser_gen.cpp"
                    break;

                    case 374:
#line 1598 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4813 "parser_gen.cpp"
                    break;

                    case 375:
#line 1603 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4821 "parser_gen.cpp"
                    break;

                    case 376:
#line 1608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4829 "parser_gen.cpp"
                    break;

                    case 377:
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4837 "parser_gen.cpp"
                    break;

                    case 378:
#line 1618 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4845 "parser_gen.cpp"
                    break;

                    case 379:
#line 1623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4853 "parser_gen.cpp"
                    break;

                    case 380:
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4859 "parser_gen.cpp"
                    break;

                    case 381:
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4865 "parser_gen.cpp"
                    break;

                    case 382:
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4871 "parser_gen.cpp"
                    break;

                    case 383:
#line 1633 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4880 "parser_gen.cpp"
                    break;

                    case 384:
#line 1640 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4889 "parser_gen.cpp"
                    break;

                    case 385:
#line 1647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4898 "parser_gen.cpp"
                    break;

                    case 386:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4904 "parser_gen.cpp"
                    break;

                    case 387:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4910 "parser_gen.cpp"
                    break;

                    case 388:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4916 "parser_gen.cpp"
                    break;

                    case 389:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4922 "parser_gen.cpp"
                    break;

                    case 390:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4928 "parser_gen.cpp"
                    break;

                    case 391:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4934 "parser_gen.cpp"
                    break;

                    case 392:
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4940 "parser_gen.cpp"
                    break;

                    case 393:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4946 "parser_gen.cpp"
                    break;

                    case 394:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4952 "parser_gen.cpp"
                    break;

                    case 395:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4958 "parser_gen.cpp"
                    break;

                    case 396:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4964 "parser_gen.cpp"
                    break;

                    case 397:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4970 "parser_gen.cpp"
                    break;

                    case 398:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4976 "parser_gen.cpp"
                    break;

                    case 399:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4982 "parser_gen.cpp"
                    break;

                    case 400:
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4988 "parser_gen.cpp"
                    break;

                    case 401:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4994 "parser_gen.cpp"
                    break;

                    case 402:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5000 "parser_gen.cpp"
                    break;

                    case 403:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5006 "parser_gen.cpp"
                    break;

                    case 404:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5012 "parser_gen.cpp"
                    break;

                    case 405:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5018 "parser_gen.cpp"
                    break;

                    case 406:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5024 "parser_gen.cpp"
                    break;

                    case 407:
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5030 "parser_gen.cpp"
                    break;

                    case 408:
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5042 "parser_gen.cpp"
                    break;

                    case 409:
#line 1670 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5050 "parser_gen.cpp"
                    break;

                    case 410:
#line 1673 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5058 "parser_gen.cpp"
                    break;

                    case 411:
#line 1679 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5066 "parser_gen.cpp"
                    break;

                    case 412:
#line 1682 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5074 "parser_gen.cpp"
                    break;

                    case 413:
#line 1689 "grammar.yy"
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
#line 5084 "parser_gen.cpp"
                    break;

                    case 414:
#line 1698 "grammar.yy"
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
#line 5094 "parser_gen.cpp"
                    break;

                    case 415:
#line 1706 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 5102 "parser_gen.cpp"
                    break;

                    case 416:
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5110 "parser_gen.cpp"
                    break;

                    case 417:
#line 1712 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5118 "parser_gen.cpp"
                    break;

                    case 418:
#line 1719 "grammar.yy"
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
#line 5130 "parser_gen.cpp"
                    break;

                    case 419:
#line 1730 "grammar.yy"
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
#line 5142 "parser_gen.cpp"
                    break;

                    case 420:
#line 1740 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 5150 "parser_gen.cpp"
                    break;

                    case 421:
#line 1743 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5158 "parser_gen.cpp"
                    break;

                    case 422:
#line 1749 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5168 "parser_gen.cpp"
                    break;

                    case 423:
#line 1757 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5178 "parser_gen.cpp"
                    break;

                    case 424:
#line 1765 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5188 "parser_gen.cpp"
                    break;

                    case 425:
#line 1773 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 5196 "parser_gen.cpp"
                    break;

                    case 426:
#line 1776 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5204 "parser_gen.cpp"
                    break;

                    case 427:
#line 1781 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 5216 "parser_gen.cpp"
                    break;

                    case 428:
#line 1790 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5224 "parser_gen.cpp"
                    break;

                    case 429:
#line 1796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5232 "parser_gen.cpp"
                    break;

                    case 430:
#line 1802 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5240 "parser_gen.cpp"
                    break;

                    case 431:
#line 1809 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 5251 "parser_gen.cpp"
                    break;

                    case 432:
#line 1819 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 5262 "parser_gen.cpp"
                    break;

                    case 433:
#line 1828 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5271 "parser_gen.cpp"
                    break;

                    case 434:
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5280 "parser_gen.cpp"
                    break;

                    case 435:
#line 1842 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5289 "parser_gen.cpp"
                    break;

                    case 436:
#line 1850 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5298 "parser_gen.cpp"
                    break;

                    case 437:
#line 1858 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5307 "parser_gen.cpp"
                    break;

                    case 438:
#line 1866 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5316 "parser_gen.cpp"
                    break;

                    case 439:
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5325 "parser_gen.cpp"
                    break;

                    case 440:
#line 1881 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5333 "parser_gen.cpp"
                    break;

                    case 441:
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5341 "parser_gen.cpp"
                    break;

                    case 442:
#line 1893 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 5349 "parser_gen.cpp"
                    break;

                    case 443:
#line 1896 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 5357 "parser_gen.cpp"
                    break;

                    case 444:
#line 1902 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5365 "parser_gen.cpp"
                    break;

                    case 445:
#line 1908 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5373 "parser_gen.cpp"
                    break;

                    case 446:
#line 1913 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5381 "parser_gen.cpp"
                    break;

                    case 447:
#line 1916 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5390 "parser_gen.cpp"
                    break;

                    case 448:
#line 1923 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 5398 "parser_gen.cpp"
                    break;

                    case 449:
#line 1926 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 5406 "parser_gen.cpp"
                    break;

                    case 450:
#line 1929 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 5414 "parser_gen.cpp"
                    break;

                    case 451:
#line 1932 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 5422 "parser_gen.cpp"
                    break;

                    case 452:
#line 1935 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 5430 "parser_gen.cpp"
                    break;

                    case 453:
#line 1938 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 5438 "parser_gen.cpp"
                    break;

                    case 454:
#line 1941 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 5446 "parser_gen.cpp"
                    break;

                    case 455:
#line 1944 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 5454 "parser_gen.cpp"
                    break;

                    case 456:
#line 1949 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5462 "parser_gen.cpp"
                    break;

                    case 457:
#line 1951 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5470 "parser_gen.cpp"
                    break;

                    case 458:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5476 "parser_gen.cpp"
                    break;

                    case 459:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5482 "parser_gen.cpp"
                    break;

                    case 460:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5488 "parser_gen.cpp"
                    break;

                    case 461:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5494 "parser_gen.cpp"
                    break;

                    case 462:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5500 "parser_gen.cpp"
                    break;

                    case 463:
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5506 "parser_gen.cpp"
                    break;

                    case 464:
#line 1958 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5512 "parser_gen.cpp"
                    break;

                    case 465:
#line 1962 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5520 "parser_gen.cpp"
                    break;

                    case 466:
#line 1968 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5528 "parser_gen.cpp"
                    break;

                    case 467:
#line 1974 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5537 "parser_gen.cpp"
                    break;

                    case 468:
#line 1982 "grammar.yy"
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
#line 5549 "parser_gen.cpp"
                    break;

                    case 469:
#line 1993 "grammar.yy"
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
#line 5561 "parser_gen.cpp"
                    break;

                    case 470:
#line 2003 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5570 "parser_gen.cpp"
                    break;

                    case 471:
#line 2011 "grammar.yy"
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
#line 5582 "parser_gen.cpp"
                    break;

                    case 472:
#line 2021 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5588 "parser_gen.cpp"
                    break;

                    case 473:
#line 2021 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5594 "parser_gen.cpp"
                    break;

                    case 474:
#line 2025 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5603 "parser_gen.cpp"
                    break;

                    case 475:
#line 2032 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5612 "parser_gen.cpp"
                    break;

                    case 476:
#line 2039 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5618 "parser_gen.cpp"
                    break;

                    case 477:
#line 2039 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5624 "parser_gen.cpp"
                    break;

                    case 478:
#line 2043 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5630 "parser_gen.cpp"
                    break;

                    case 479:
#line 2043 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5636 "parser_gen.cpp"
                    break;

                    case 480:
#line 2047 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5644 "parser_gen.cpp"
                    break;

                    case 481:
#line 2053 "grammar.yy"
                    {
                    }
#line 5650 "parser_gen.cpp"
                    break;

                    case 482:
#line 2054 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 5659 "parser_gen.cpp"
                    break;

                    case 483:
#line 2061 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5667 "parser_gen.cpp"
                    break;

                    case 484:
#line 2067 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5675 "parser_gen.cpp"
                    break;

                    case 485:
#line 2070 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5684 "parser_gen.cpp"
                    break;

                    case 486:
#line 2077 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5692 "parser_gen.cpp"
                    break;

                    case 487:
#line 2084 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5698 "parser_gen.cpp"
                    break;

                    case 488:
#line 2085 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5704 "parser_gen.cpp"
                    break;

                    case 489:
#line 2086 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5710 "parser_gen.cpp"
                    break;

                    case 490:
#line 2087 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5716 "parser_gen.cpp"
                    break;

                    case 491:
#line 2088 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5722 "parser_gen.cpp"
                    break;

                    case 492:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5728 "parser_gen.cpp"
                    break;

                    case 493:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5734 "parser_gen.cpp"
                    break;

                    case 494:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5740 "parser_gen.cpp"
                    break;

                    case 495:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5746 "parser_gen.cpp"
                    break;

                    case 496:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5752 "parser_gen.cpp"
                    break;

                    case 497:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5758 "parser_gen.cpp"
                    break;

                    case 498:
#line 2091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5764 "parser_gen.cpp"
                    break;

                    case 499:
#line 2093 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5773 "parser_gen.cpp"
                    break;

                    case 500:
#line 2098 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5782 "parser_gen.cpp"
                    break;

                    case 501:
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5791 "parser_gen.cpp"
                    break;

                    case 502:
#line 2108 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5800 "parser_gen.cpp"
                    break;

                    case 503:
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5809 "parser_gen.cpp"
                    break;

                    case 504:
#line 2118 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5818 "parser_gen.cpp"
                    break;

                    case 505:
#line 2123 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5827 "parser_gen.cpp"
                    break;

                    case 506:
#line 2129 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5833 "parser_gen.cpp"
                    break;

                    case 507:
#line 2130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5839 "parser_gen.cpp"
                    break;

                    case 508:
#line 2131 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5845 "parser_gen.cpp"
                    break;

                    case 509:
#line 2132 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5851 "parser_gen.cpp"
                    break;

                    case 510:
#line 2133 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5857 "parser_gen.cpp"
                    break;

                    case 511:
#line 2134 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5863 "parser_gen.cpp"
                    break;

                    case 512:
#line 2135 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5869 "parser_gen.cpp"
                    break;

                    case 513:
#line 2136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5875 "parser_gen.cpp"
                    break;

                    case 514:
#line 2137 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5881 "parser_gen.cpp"
                    break;

                    case 515:
#line 2138 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5887 "parser_gen.cpp"
                    break;

                    case 516:
#line 2143 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 5895 "parser_gen.cpp"
                    break;

                    case 517:
#line 2146 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5903 "parser_gen.cpp"
                    break;

                    case 518:
#line 2153 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 5911 "parser_gen.cpp"
                    break;

                    case 519:
#line 2156 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5919 "parser_gen.cpp"
                    break;

                    case 520:
#line 2163 "grammar.yy"
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
#line 5930 "parser_gen.cpp"
                    break;

                    case 521:
#line 2172 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5938 "parser_gen.cpp"
                    break;

                    case 522:
#line 2177 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5946 "parser_gen.cpp"
                    break;

                    case 523:
#line 2182 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5954 "parser_gen.cpp"
                    break;

                    case 524:
#line 2187 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5962 "parser_gen.cpp"
                    break;

                    case 525:
#line 2192 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5970 "parser_gen.cpp"
                    break;

                    case 526:
#line 2197 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5978 "parser_gen.cpp"
                    break;

                    case 527:
#line 2202 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5986 "parser_gen.cpp"
                    break;

                    case 528:
#line 2207 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5994 "parser_gen.cpp"
                    break;

                    case 529:
#line 2212 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6002 "parser_gen.cpp"
                    break;


#line 6006 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -798;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    -39,  -72,  -64,  -62,  54,   -59,  -798, -798, -798, -798, -798, -798, 57,   11,   1130, 728,
    -50,  65,   -49,  -47,  65,   -798, 21,   -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, 2558, -798, -798, -798, -38,  -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, 84,   -798, 23,   -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, 70,   -798, 78,   -37,  -59,  -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, 18,   -798, -798, -798, 2803, 65,   311,  -798, -798, -24,
    -64,  -66,  -798, 2688, -798, -798, 2688, -798, -798, -798, -798, 47,   77,   -798, -104, -798,
    -798, 51,   -798, -798, 53,   -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, 564,  -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, 71,   -798, -798, -798,
    -798, 1128, 2168, 2298, 2298, -7,   -2,   -7,   3,    2298, 2298, 2298, 12,   2298, 2168, 12,
    20,   25,   -798, 2298, 2298, -798, -798, 2298, 26,   12,   2168, 2168, 12,   12,   -798, 29,
    30,   32,   2168, 33,   2168, 12,   12,   -798, 138,  36,   37,   12,   38,   -7,   39,   2298,
    -798, -798, -798, -798, -798, 40,   -798, 12,   43,   44,   12,   49,   60,   2298, 2298, 63,
    2168, 68,   2168, 2168, 69,   73,   74,   79,   2298, 2298, 2168, 2168, 2168, 2168, 2168, 2168,
    2168, 2168, 2168, 2168, -798, 82,   2168, 2688, 2688, -798, 2853, 59,   98,   -798, 1000, -798,
    -798, -798, -798, -798, 109,  2168, -798, -798, -798, -798, -798, -798, 129,  133,  145,  2168,
    146,  2168, 148,  150,  151,  2168, 152,  155,  156,  157,  -798, 2428, 86,   158,  160,  199,
    201,  163,  2168, 164,  165,  166,  167,  177,  2168, 2168, 2428, 178,  2168, 179,  180,  186,
    229,  194,  198,  205,  206,  208,  212,  217,  220,  221,  2168, 2168, 222,  2168, 224,  2168,
    225,  236,  279,  255,  256,  295,  298,  2168, 229,  262,  2168, 2168, 263,  2168, 2168, 264,
    268,  270,  271,  2168, 272,  2168, 273,  274,  2168, 2168, 2168, 2168, 275,  276,  277,  278,
    280,  283,  284,  291,  296,  297,  299,  300,  229,  2168, 302,  -798, -798, -798, -798, -798,
    303,  2869, -798, 305,  -798, -798, -798, 307,  -798, 308,  -798, -798, -798, 2168, -798, -798,
    -798, -798, 1258, -798, -798, 312,  -798, -798, -798, -798, 2168, -798, -798, 2168, 2168, -798,
    2168, -798, -798, -798, -798, -798, 2168, 2168, 318,  -798, 2168, -798, -798, -798, 2168, 314,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, 2168, 2168, -798, 319,  -798, 2168, -798,
    -798, 2168, -798, -798, 2168, 2168, 2168, 330,  -798, 2168, 2168, -798, 2168, 2168, -798, -798,
    -798, -798, 2168, -798, 2168, -798, -798, 2168, 2168, 2168, 2168, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, 333,  2168, -798, -798, -798, 2168, -798, -798, -798,
    -798, -798, -798, 324,  326,  329,  332,  1388, 864,  334,  369,  372,  372,  338,  2168, 2168,
    339,  341,  -798, 2168, 343,  -798, 347,  349,  370,  384,  385,  352,  2168, -798, -798, -798,
    1518, 353,  357,  2168, 2168, 2168, 359,  2168, 360,  -798, -798, -798, -798, -798, -798, -798,
    -798, 2428, -798, -798, 2168, 393,  2168, 390,  390,  365,  2168, 368,  374,  -798, 375,  376,
    378,  1648, -798, 379,  2168, 398,  2168, 2168, 380,  381,  1778, 1908, 2038, 386,  388,  389,
    387,  391,  392,  394,  396,  397,  -798, -798, 2168, 400,  -798, 2168, 369,  393,  -798, -798,
    403,  406,  -798, 407,  -798, 408,  -798, -798, 2168, 421,  422,  -798, 409,  410,  411,  412,
    -798, -798, -798, 413,  416,  417,  -798, 418,  -798, -798, 2168, -798, 393,  419,  -798, -798,
    -798, -798, 423,  2168, 2168, -798, -798, -798, -798, -798, -798, -798, -798, 425,  426,  427,
    -798, 428,  429,  437,  438,  -798, 439,  440,  -798, -798, -798, -798};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   75,  3,   446, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   10,  11,  12,  13,  14,  15,  5,   87,  115, 104, 114, 111, 118, 112, 107,
    109, 110, 117, 105, 116, 119, 106, 113, 108, 74,  307, 89,  88,  95,  93,  94,  92,  0,   102,
    76,  78,  0,   144, 197, 200, 120, 183, 122, 184, 196, 199, 198, 121, 201, 145, 127, 160, 123,
    134, 191, 194, 161, 162, 202, 146, 445, 128, 147, 148, 129, 130, 163, 164, 124, 149, 150, 151,
    131, 132, 165, 166, 152, 153, 133, 126, 125, 154, 203, 167, 168, 169, 171, 170, 155, 172, 185,
    186, 187, 188, 189, 156, 190, 193, 173, 157, 96,  99,  100, 101, 98,  97,  176, 174, 175, 177,
    178, 179, 158, 192, 195, 135, 136, 137, 138, 139, 140, 180, 141, 142, 182, 181, 159, 143, 488,
    489, 490, 487, 491, 0,   447, 0,   244, 243, 242, 240, 239, 238, 232, 231, 230, 236, 235, 234,
    229, 233, 237, 241, 19,  20,  21,  22,  24,  26,  0,   23,  0,   0,   6,   246, 245, 205, 206,
    207, 208, 209, 210, 211, 212, 81,  213, 204, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 256, 257, 258, 259, 260, 265, 261, 262, 263, 266, 267, 247, 248, 250,
    251, 252, 264, 253, 254, 255, 79,  249, 77,  90,  455, 454, 453, 452, 449, 448, 451, 450, 0,
    456, 457, 17,  0,   0,   0,   9,   7,   0,   0,   0,   25,  0,   66,  68,  0,   65,  67,  27,
    103, 0,   0,   80,  0,   82,  83,  91,  442, 443, 0,   59,  58,  55,  54,  57,  51,  50,  53,
    43,  42,  45,  47,  46,  49,  268, 0,   44,  48,  52,  56,  38,  39,  40,  41,  60,  61,  62,
    31,  32,  33,  34,  35,  36,  37,  28,  30,  63,  64,  278, 292, 290, 279, 280, 309, 281, 380,
    381, 382, 282, 472, 473, 285, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398,
    399, 400, 401, 402, 403, 404, 405, 407, 406, 283, 492, 493, 494, 495, 496, 497, 498, 284, 506,
    507, 508, 509, 510, 511, 512, 513, 514, 515, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319,
    320, 321, 322, 323, 324, 286, 458, 459, 460, 461, 462, 463, 464, 287, 334, 335, 336, 337, 338,
    339, 340, 341, 342, 344, 345, 346, 343, 347, 348, 291, 29,  16,  0,   81,  84,  86,  444, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8,   0,   0,
    8,   8,   0,   0,   0,   0,   0,   0,   0,   308, 0,   0,   0,   0,   0,   0,   0,   0,   8,
    0,   0,   0,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   0,   8,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   8,   0,   0,   0,   0,   70,  0,   0,   0,   295, 300, 270, 269,
    272, 271, 273, 0,   0,   274, 276, 297, 275, 277, 298, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   268, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,   73,  72,  69,  71,  18,  82,  0,   351,
    0,   373, 376, 349, 0,   383, 0,   372, 375, 374, 0,   350, 377, 352, 499, 0,   481, 484, 0,
    476, 477, 478, 479, 0,   367, 370, 0,   0,   378, 0,   500, 354, 355, 501, 502, 0,   0,   0,
    356, 0,   358, 503, 504, 0,   0,   325, 326, 327, 328, 329, 330, 331, 332, 333, 0,   0,   505,
    0,   384, 0,   379, 428, 0,   429, 430, 0,   0,   0,   0,   467, 0,   0,   470, 0,   0,   293,
    294, 366, 369, 0,   363, 0,   434, 435, 0,   0,   0,   0,   368, 371, 521, 522, 523, 524, 525,
    526, 440, 527, 528, 441, 0,   0,   529, 85,  299, 0,   304, 305, 303, 306, 301, 296, 0,   0,
    0,   0,   0,   0,   0,   516, 409, 409, 0,   415, 415, 0,   0,   421, 0,   0,   268, 0,   0,
    425, 0,   0,   0,   0,   268, 268, 268, 0,   0,   0,   0,   0,   0,   0,   0,   0,   302, 465,
    466, 288, 408, 480, 482, 483, 0,   485, 474, 0,   518, 0,   411, 411, 0,   416, 0,   0,   475,
    0,   0,   0,   0,   385, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   486, 517, 0,   0,   410, 0,   516, 518, 353, 417, 0,   0,   357, 0,
    359, 0,   361, 426, 0,   0,   0,   362, 0,   0,   0,   0,   289, 433, 436, 0,   0,   0,   364,
    0,   365, 519, 0,   412, 518, 0,   418, 419, 422, 360, 0,   0,   0,   423, 468, 469, 471, 437,
    438, 439, 424, 0,   0,   0,   427, 0,   0,   0,   0,   414, 0,   0,   520, 413, 432, 431};

const short ParserGen::yypgoto_[] = {
    -798, 245,  -798, -798, -176, -13,  -798, -798, -12,  -11,  -798, -246, -798, -798, -6,
    -798, -798, -245, -225, -220, -213, -211, -3,   -206, 4,    -8,   9,    -200, -193, -539,
    -239, -798, -186, -171, -167, -798, -163, -159, -237, -55,  -798, -798, -798, -798, -798,
    -798, 316,  -798, -798, -798, -798, -798, -798, -798, -798, -798, 242,  -468, -798, -4,
    -412, -798, 66,   -798, -798, -798, -251, 67,   -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -422,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -323, -797, -240, -280, -556, -798, -531, -798, -238, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798, -798,
    -798, -798, -798, -798, -798, -798, -798, 46,   -798, 906,  258,  -798, 102,  -798, -798,
    -798, -798, 8,    -798, -798, -798, -798, -798, -798, -798, -798, -798, -17,  -798};

const short ParserGen::yydefgoto_[] = {
    -1,  503, 263, 732, 151, 152, 264, 153, 154, 155, 504, 156, 55,  265, 505, 737, 786, 56,
    214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 645, 225, 226, 227, 228, 229, 230,
    231, 232, 233, 511, 647, 648, 649, 744, 235, 6,   13,  22,  23,  24,  25,  26,  27,  28,
    250, 506, 311, 312, 313, 179, 512, 314, 534, 592, 315, 316, 513, 514, 625, 318, 319, 320,
    321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 577,
    338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355,
    356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373,
    374, 375, 376, 377, 378, 379, 380, 381, 382, 383, 384, 789, 825, 791, 828, 671, 805, 417,
    743, 795, 385, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399, 400,
    401, 402, 403, 404, 405, 406, 407, 408, 520, 521, 515, 523, 524, 8,   14,  255, 236, 256,
    57,  58,  271, 272, 59,  10,  15,  247, 248, 276, 157, 4,   578, 184};

const short ParserGen::yytable_[] = {
    234, 52,  53,  54,  183, 266, 516, 642, 317, 177, 413, 317, 177, 268, 175, 664, 182, 175, 536,
    274, 304, 176, 310, 304, 176, 310, 178, 269, 695, 178, 548, 549, 861, 414, 297, 619, 620, 297,
    555, 298, 557, 5,   298, 579, 580, 206, 299, 266, 300, 299, 7,   300, 9,   301, 11,  12,  301,
    270, 275, 302, 727, 29,  302, 878, 158, 180, 303, 181, 596, 303, 598, 599, 185, 305, 249, 237,
    305, 253, 606, 607, 608, 609, 610, 611, 612, 613, 614, 615, 306, 252, 618, 306, 307, 251, 257,
    307, 308, 412, 411, 308, 309, 415, 650, 309, 416, 627, 291, 159, 160, 161, 623, 527, 162, 163,
    164, 631, 529, 633, 162, 163, 164, 637, 1,   2,   3,   533, 238, 239, 165, 166, 167, 240, 241,
    538, 656, 168, 169, 170, 539, 546, 662, 663, 552, 553, 666, 554, 556, 242, 243, 570, 571, 573,
    575, 583, 244, 245, 586, 587, 681, 682, 626, 684, 589, 686, 16,  17,  18,  19,  20,  21,  526,
    694, 528, 590, 697, 698, 595, 700, 701, 270, 628, 597, 600, 706, 629, 708, 601, 602, 711, 712,
    713, 714, 603, 561, 562, 617, 630, 632, 246, 634, 563, 635, 636, 638, 783, 728, 639, 640, 641,
    651, 574, 652, 653, 654, 655, 657, 658, 659, 660, 171, 172, 173, 174, 564, 565, 741, 801, 173,
    661, 665, 667, 668, 566, 567, 810, 811, 812, 669, 746, 670, 568, 747, 748, 177, 749, 672, 822,
    267, 175, 673, 750, 751, 317, 317, 753, 176, 674, 675, 754, 676, 178, 266, 569, 677, 304, 304,
    310, 310, 678, 756, 757, 679, 680, 683, 759, 685, 687, 760, 297, 297, 761, 762, 763, 298, 298,
    765, 766, 688, 767, 768, 299, 299, 300, 300, 769, 689, 770, 301, 301, 771, 772, 773, 774, 302,
    302, 409, 690, 691, 409, 692, 303, 303, 693, 696, 699, 702, 776, 305, 305, 703, 777, 704, 705,
    707, 709, 710, 715, 716, 717, 718, 755, 719, 306, 306, 720, 721, 307, 307, 794, 794, 308, 308,
    722, 799, 309, 309, 764, 723, 724, 775, 725, 726, 809, 729, 730, 738, 813, 739, 740, 816, 817,
    818, 745, 820, 518, 518, 188, 189, 752, 758, 518, 518, 518, 190, 518, 778, 823, 779, 826, 780,
    518, 518, 831, 781, 518, 787, 788, 790, 793, 804, 797, 798, 839, 800, 841, 842, 191, 192, 802,
    803, 806, 807, 808, 814, 507, 193, 194, 815, 518, 819, 821, 824, 857, 195, 827, 859, 830, 540,
    832, 840, 543, 544, 518, 518, 833, 858, 834, 835, 866, 836, 838, 843, 844, 518, 518, 197, 848,
    851, 560, 849, 850, 852, 853, 867, 868, 854, 877, 855, 856, 733, 581, 582, 198, 584, 862, 881,
    882, 863, 864, 865, 869, 870, 871, 872, 873, 522, 522, 874, 875, 876, 879, 522, 522, 522, 880,
    522, 883, 884, 885, 886, 887, 522, 522, 616, 646, 522, 519, 519, 888, 889, 890, 891, 519, 519,
    519, 262, 519, 537, 785, 646, 622, 254, 519, 519, 410, 860, 519, 547, 792, 522, 550, 551, 829,
    796, 273, 508, 624, 0,   0,   558, 559, 0,   0,   522, 522, 572, 0,   0,   0,   0,   519, 0,
    0,   0,   522, 522, 0,   585, 0,   0,   588, 0,   591, 0,   519, 519, 0,   0,   0,   409, 409,
    0,   0,   0,   0,   519, 519, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   418, 419, 420,
    421, 422, 423, 424, 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,
    46,  425, 426, 427, 428, 429, 0,   0,   430, 431, 432, 433, 434, 435, 436, 437, 438, 0,   0,
    0,   439, 440, 0,   734, 735, 736, 0,   441, 442, 443, 0,   0,   444, 445, 446, 447, 448, 0,
    0,   0,   0,   449, 450, 451, 452, 0,   0,   0,   453, 454, 455, 456, 457, 458, 459, 0,   460,
    461, 462, 463, 0,   0,   464, 465, 466, 467, 468, 469, 470, 0,   0,   471, 472, 473, 474, 475,
    476, 0,   477, 478, 479, 480, 0,   0,   0,   0,   0,   0,   0,   0,   481, 482, 483, 484, 485,
    486, 487, 488, 489, 646, 490, 491, 492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 260,
    261, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   646, 60,  61,  62,  63,  64,  65,  66,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  67,  68,  69,  70,  71,  0,
    0,   72,  73,  74,  75,  76,  77,  78,  79,  80,  0,   0,   0,   81,  82,  0,   0,   0,   0,
    83,  84,  85,  86,  0,   0,   87,  88,  48,  89,  90,  0,   0,   0,   0,   91,  92,  93,  94,
    0,   0,   0,   95,  96,  97,  98,  99,  100, 101, 0,   102, 103, 104, 105, 0,   0,   106, 107,
    108, 109, 110, 111, 112, 0,   0,   113, 114, 115, 116, 117, 118, 0,   119, 120, 121, 122, 123,
    124, 125, 126, 127, 128, 0,   0,   129, 130, 131, 132, 133, 134, 135, 136, 137, 0,   138, 139,
    140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 51,  60,  61,  62,  63,  64,  65,  66,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  67,  68,  69,
    70,  71,  0,   0,   72,  73,  74,  75,  76,  77,  78,  79,  80,  0,   0,   0,   81,  82,  0,
    0,   0,   0,   784, 84,  85,  86,  0,   0,   87,  88,  48,  89,  90,  0,   0,   0,   0,   91,
    92,  93,  94,  0,   0,   0,   95,  96,  97,  98,  99,  100, 101, 0,   102, 103, 104, 105, 0,
    0,   106, 107, 108, 109, 110, 111, 112, 0,   0,   113, 114, 115, 116, 117, 118, 0,   119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 0,   0,   129, 130, 131, 132, 133, 134, 135, 136, 137,
    0,   138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 51,  418, 419, 420, 421,
    422, 423, 424, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    425, 426, 427, 428, 429, 0,   0,   430, 431, 432, 433, 434, 435, 436, 437, 438, 0,   0,   0,
    439, 440, 0,   0,   0,   0,   0,   441, 442, 443, 0,   0,   444, 445, 0,   447, 448, 0,   0,
    0,   0,   449, 450, 451, 452, 0,   0,   0,   453, 454, 455, 456, 457, 458, 459, 0,   460, 461,
    462, 463, 0,   0,   464, 465, 466, 467, 468, 469, 470, 0,   0,   471, 472, 473, 474, 475, 476,
    0,   477, 478, 479, 480, 0,   0,   0,   0,   0,   0,   0,   0,   481, 482, 483, 484, 485, 486,
    487, 488, 489, 0,   490, 491, 492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 30,  0,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  0,   0,   0,
    186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164,
    509, 0,   0,   47,  0,   188, 189, 0,   0,   0,   0,   48,  190, 165, 166, 167, 0,   0,   0,
    0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,   0,   0,   49,  0,   50,  191, 192, 0,
    0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   291, 510, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   51,  198, 199, 200, 201,
    202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 742, 0,   0,
    0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 165, 166, 167, 0,   0,   525, 0,   168, 169,
    170, 530, 531, 532, 0,   535, 0,   0,   0,   0,   0,   541, 542, 191, 192, 545, 0,   0,   0,
    0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,
    0,   576, 0,   291, 510, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 593, 594, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   604, 605, 0,   0,   0,   198, 199, 200, 201, 202, 203, 204,
    205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 782, 0,   0,   0,   0,   188,
    189, 0,   0,   0,   0,   0,   190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,
    193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    643, 644, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 780, 0,   0,   0,   0,   188, 189, 0,   0,
    0,   0,   0,   190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,
    0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   291, 510, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210,
    171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,   159,
    160, 161, 0,   0,   162, 163, 164, 837, 0,   0,   0,   0,   188, 189, 0,   0,   0,   0,   0,
    190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,
    0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   291, 510, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172, 173,
    174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161, 0,
    0,   162, 163, 164, 845, 0,   0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 165, 166,
    167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   291, 510, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211, 212,
    213, 186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163,
    164, 846, 0,   0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 165, 166, 167, 0,   0,
    0,   0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   191, 192,
    0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   291, 510, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   198, 199, 200,
    201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 847, 0,
    0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 165, 166, 167, 0,   0,   0,   0,   168,
    169, 170, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   191, 192, 0,   0,   0,
    0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   291, 510, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   198, 199, 200, 201, 202, 203,
    204, 205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 0,   0,   0,   0,   0,
    188, 189, 0,   0,   0,   0,   0,   190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,
    0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   291, 510, 0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206,
    207, 208, 209, 210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   159, 160, 161, 0,   0,   162, 163, 164, 0,   0,   0,   0,   0,   188, 189, 0,
    0,   0,   0,   0,   190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194,
    0,   0,   0,   0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   517, 510,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 171, 172, 173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    159, 160, 161, 0,   0,   162, 163, 164, 0,   0,   0,   0,   0,   188, 189, 0,   0,   0,   0,
    0,   190, 165, 166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,
    0,   0,   0,   195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   643, 644, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172,
    173, 174, 211, 212, 213, 186, 187, 0,   0,   0,   0,   0,   0,   0,   0,   0,   159, 160, 161,
    0,   0,   162, 163, 164, 0,   0,   0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 165,
    166, 167, 0,   0,   0,   0,   168, 169, 170, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   191, 192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,
    195, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   196, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 171, 172, 173, 174, 211,
    212, 213, 277, 278, 0,   0,   0,   0,   0,   0,   0,   0,   0,   279, 280, 281, 0,   0,   282,
    283, 284, 0,   0,   0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   190, 285, 286, 287, 0,
    0,   0,   0,   288, 289, 290, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   191,
    192, 0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   195, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   291, 292, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    197, 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  198, 0,
    0,   201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 293, 294, 295, 296, 211, 212, 213, 0,
    0,   0,   0,   0,   258, 0,   0,   0,   0,   0,   0,   0,   259, 31,  32,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  0,   0,   0,   0,   0,   0,   0,   0,   0,   621, 0,   0,
    0,   0,   0,   0,   0,   446, 0,   0,   0,   0,   0,   0,   0,   731, 0,   0,   0,   0,   0,
    0,   0,   48,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   260, 261, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   123, 124, 125, 126, 127, 128, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   260, 261, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   51};

const short ParserGen::yycheck_[] = {
    55,  14,  14,  14,  21,  250, 418, 538, 259, 17,  114, 262, 20,  252, 17,  554, 20,  20,  430,
    85,  259, 17,  259, 262, 20,  262, 17,  51,  584, 20,  442, 443, 829, 270, 259, 503, 504, 262,
    450, 259, 452, 113, 262, 465, 466, 149, 259, 292, 259, 262, 114, 262, 114, 259, 0,   114, 262,
    81,  124, 259, 616, 50,  262, 860, 114, 114, 259, 114, 480, 262, 482, 483, 51,  259, 51,  113,
    262, 114, 490, 491, 492, 493, 494, 495, 496, 497, 498, 499, 259, 11,  502, 262, 259, 23,  76,
    262, 259, 20,  51,  262, 259, 50,  16,  262, 51,  517, 113, 42,  43,  44,  51,  113, 47,  48,
    49,  527, 113, 529, 47,  48,  49,  533, 161, 162, 163, 113, 42,  43,  63,  64,  65,  47,  48,
    113, 546, 70,  71,  72,  113, 113, 552, 553, 113, 113, 556, 113, 113, 63,  64,  113, 113, 113,
    113, 113, 70,  71,  113, 113, 570, 571, 51,  573, 113, 575, 107, 108, 109, 110, 111, 112, 421,
    583, 423, 113, 586, 587, 113, 589, 590, 81,  51,  113, 113, 595, 51,  597, 113, 113, 600, 601,
    602, 603, 113, 55,  56,  113, 51,  51,  114, 51,  62,  51,  51,  51,  743, 617, 51,  51,  51,
    51,  461, 51,  13,  12,  51,  51,  51,  51,  51,  154, 155, 156, 157, 85,  86,  637, 757, 156,
    51,  51,  51,  51,  94,  95,  765, 766, 767, 51,  650, 10,  102, 653, 654, 251, 656, 51,  785,
    251, 251, 51,  662, 663, 503, 504, 666, 251, 51,  51,  670, 51,  251, 506, 124, 51,  503, 504,
    503, 504, 51,  681, 682, 51,  51,  51,  686, 51,  51,  689, 503, 504, 692, 693, 694, 503, 504,
    697, 698, 51,  700, 701, 503, 504, 503, 504, 706, 16,  708, 503, 504, 711, 712, 713, 714, 503,
    504, 259, 51,  51,  262, 14,  503, 504, 14,  51,  51,  51,  728, 503, 504, 51,  732, 51,  51,
    51,  51,  51,  51,  51,  51,  51,  16,  51,  503, 504, 51,  51,  503, 504, 750, 751, 503, 504,
    51,  755, 503, 504, 16,  51,  51,  16,  51,  51,  764, 51,  51,  50,  768, 50,  50,  771, 772,
    773, 50,  775, 419, 420, 55,  56,  50,  50,  425, 426, 427, 62,  429, 51,  788, 51,  790, 50,
    435, 436, 794, 51,  439, 51,  17,  15,  50,  19,  51,  50,  804, 50,  806, 807, 85,  86,  51,
    50,  16,  16,  50,  50,  412, 94,  95,  50,  463, 50,  50,  18,  824, 102, 24,  827, 51,  434,
    50,  21,  437, 438, 477, 478, 50,  25,  51,  51,  840, 51,  51,  51,  51,  488, 489, 124, 50,
    50,  455, 51,  51,  50,  50,  22,  22,  51,  858, 51,  51,  625, 467, 468, 141, 470, 51,  867,
    868, 51,  51,  51,  51,  51,  51,  51,  51,  419, 420, 51,  51,  51,  51,  425, 426, 427, 51,
    429, 51,  51,  51,  51,  51,  435, 436, 500, 539, 439, 419, 420, 51,  51,  51,  51,  425, 426,
    427, 250, 429, 431, 744, 554, 506, 185, 435, 436, 262, 828, 439, 441, 748, 463, 444, 445, 792,
    751, 256, 413, 508, -1,  -1,  453, 454, -1,  -1,  477, 478, 459, -1,  -1,  -1,  -1,  463, -1,
    -1,  -1,  488, 489, -1,  471, -1,  -1,  474, -1,  476, -1,  477, 478, -1,  -1,  -1,  503, 504,
    -1,  -1,  -1,  -1,  488, 489, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  3,   4,   5,
    6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,
    -1,  45,  46,  -1,  625, 625, 625, -1,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,  61,  -1,
    -1,  -1,  -1,  66,  67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,
    82,  83,  84,  -1,  -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100,
    101, -1,  103, 104, 105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, 117, 118, 119,
    120, 121, 122, 123, 743, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
    139, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  785, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  -1,
    -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,  -1,  45,  46,  -1,  -1,  -1,  -1,
    51,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  66,  67,  68,  69,
    -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,  82,  83,  84,  -1,  -1,  87,  88,
    89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100, 101, -1,  103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, -1,  -1,  115, 116, 117, 118, 119, 120, 121, 122, 123, -1,  125, 126,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,
    29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,  -1,  45,  46,  -1,
    -1,  -1,  -1,  51,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  66,
    67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,  82,  83,  84,  -1,
    -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100, 101, -1,  103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, -1,  -1,  115, 116, 117, 118, 119, 120, 121, 122, 123,
    -1,  125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 3,   4,   5,   6,
    7,   8,   9,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    26,  27,  28,  29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,  -1,
    45,  46,  -1,  -1,  -1,  -1,  -1,  52,  53,  54,  -1,  -1,  57,  58,  -1,  60,  61,  -1,  -1,
    -1,  -1,  66,  67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,  82,
    83,  84,  -1,  -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100, 101,
    -1,  103, 104, 105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, 117, 118, 119, 120,
    121, 122, 123, -1,  125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 8,   -1,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,
    31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,
    50,  -1,  -1,  51,  -1,  55,  56,  -1,  -1,  -1,  -1,  59,  62,  63,  64,  65,  -1,  -1,  -1,
    -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  80,  -1,  82,  85,  86,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138, 141, 142, 143, 144,
    145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  50,  -1,  -1,
    -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  420, -1,  70,  71,
    72,  425, 426, 427, -1,  429, -1,  -1,  -1,  -1,  -1,  435, 436, 85,  86,  439, -1,  -1,  -1,
    -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  463, -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, 477, 478, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  488, 489, -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147,
    148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  50,  -1,  -1,  -1,  -1,  55,
    56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150,
    151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  50,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,
    -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,
    -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,
    43,  44,  -1,  -1,  47,  48,  49,  50,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,
    62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,
    -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156,
    157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,
    -1,  47,  48,  49,  50,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,
    65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,
    49,  50,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,
    -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  50,  -1,
    -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,
    71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146,
    147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  -1,  -1,  -1,  -1,  -1,
    55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  -1,  -1,  -1,  -1,  -1,  55,  56,  -1,
    -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,
    -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152,
    153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    42,  43,  44,  -1,  -1,  47,  48,  49,  -1,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,
    -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,
    -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,
    -1,  -1,  47,  48,  49,  -1,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,
    64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,
    102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,
    48,  49,  -1,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,
    -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,
    86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    124, 10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  141, -1,
    -1,  144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, -1,
    -1,  -1,  -1,  -1,  51,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  59,  10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  10,  11,  12,  13,  14,  15,  16,  17,  18,
    19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  51,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  59,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  51,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  59,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138, 139, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  107, 108, 109, 110, 111, 112, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138, 139, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  138};

const short ParserGen::yystos_[] = {
    0,   161, 162, 163, 355, 113, 209, 114, 339, 114, 349, 0,   114, 210, 340, 350, 107, 108, 109,
    110, 111, 112, 211, 212, 213, 214, 215, 216, 217, 50,  8,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  25,  51,  59,  80,  82,  138, 169, 172, 173, 176, 181,
    344, 345, 348, 3,   4,   5,   6,   7,   8,   9,   26,  27,  28,  29,  30,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  45,  46,  51,  52,  53,  54,  57,  58,  60,  61,  66,  67,  68,  69,
    73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  87,  88,  89,  90,  91,  92,  93,  96,
    97,  98,  99,  100, 101, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 168,
    169, 171, 172, 173, 175, 354, 114, 42,  43,  44,  47,  48,  49,  63,  64,  65,  70,  71,  72,
    154, 155, 156, 157, 186, 188, 189, 190, 223, 114, 114, 223, 356, 357, 51,  31,  32,  55,  56,
    62,  85,  86,  94,  95,  102, 114, 124, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151,
    152, 153, 158, 159, 160, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 194, 195, 196,
    197, 198, 199, 200, 201, 202, 203, 208, 342, 113, 42,  43,  47,  48,  63,  64,  70,  71,  114,
    351, 352, 51,  218, 23,  11,  114, 210, 341, 343, 76,  51,  59,  138, 139, 165, 166, 170, 177,
    181, 223, 194, 51,  81,  346, 347, 339, 85,  124, 353, 31,  32,  42,  43,  44,  47,  48,  49,
    63,  64,  65,  70,  71,  72,  113, 114, 154, 155, 156, 157, 182, 183, 184, 185, 187, 191, 192,
    194, 196, 197, 198, 200, 201, 202, 220, 221, 222, 225, 228, 229, 230, 233, 234, 235, 236, 237,
    238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 254, 255, 256, 257,
    258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276,
    277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295,
    296, 297, 298, 299, 300, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323,
    324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 336, 220, 51,  20,  114, 202, 50,  51,  307,
    3,   4,   5,   6,   7,   8,   9,   26,  27,  28,  29,  30,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  45,  46,  52,  53,  54,  57,  58,  59,  60,  61,  66,  67,  68,  69,  73,  74,  75,
    76,  77,  78,  79,  81,  82,  83,  84,  87,  88,  89,  90,  91,  92,  93,  96,  97,  98,  99,
    100, 101, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 165, 174, 178, 219, 189, 341, 50,  114, 203, 224,
    230, 231, 336, 224, 113, 203, 231, 334, 335, 336, 337, 338, 338, 230, 113, 230, 113, 338, 338,
    338, 113, 226, 338, 224, 226, 113, 113, 356, 338, 338, 356, 356, 338, 113, 226, 224, 224, 226,
    226, 113, 113, 113, 224, 113, 224, 226, 226, 356, 55,  56,  62,  85,  86,  94,  95,  102, 124,
    113, 113, 226, 113, 230, 113, 338, 253, 356, 253, 253, 356, 356, 113, 356, 226, 113, 113, 226,
    113, 113, 226, 227, 338, 338, 113, 224, 113, 224, 224, 113, 113, 113, 113, 338, 338, 224, 224,
    224, 224, 224, 224, 224, 224, 224, 224, 356, 113, 224, 221, 221, 51,  178, 51,  346, 232, 51,
    224, 51,  51,  51,  224, 51,  224, 51,  51,  51,  224, 51,  51,  51,  51,  307, 113, 114, 193,
    203, 204, 205, 206, 16,  51,  51,  13,  12,  51,  224, 51,  51,  51,  51,  51,  224, 224, 193,
    51,  224, 51,  51,  51,  10,  305, 51,  51,  51,  51,  51,  51,  51,  51,  51,  224, 224, 51,
    224, 51,  224, 51,  51,  16,  51,  51,  14,  14,  224, 305, 51,  224, 224, 51,  224, 224, 51,
    51,  51,  51,  224, 51,  224, 51,  51,  224, 224, 224, 224, 51,  51,  51,  51,  51,  51,  51,
    51,  51,  51,  51,  51,  305, 224, 51,  51,  51,  167, 168, 169, 172, 173, 179, 50,  50,  50,
    224, 50,  308, 207, 50,  224, 224, 224, 224, 224, 224, 50,  224, 224, 16,  224, 224, 50,  224,
    224, 224, 224, 224, 16,  224, 224, 224, 224, 224, 224, 224, 224, 224, 224, 16,  224, 224, 51,
    51,  50,  51,  50,  193, 51,  175, 180, 51,  17,  301, 15,  303, 303, 50,  224, 309, 309, 51,
    50,  224, 50,  307, 51,  50,  19,  306, 16,  16,  50,  224, 307, 307, 307, 224, 50,  50,  224,
    224, 224, 50,  224, 50,  193, 224, 18,  302, 224, 24,  304, 304, 51,  224, 50,  50,  51,  51,
    51,  50,  51,  224, 21,  224, 224, 51,  51,  50,  50,  50,  50,  51,  51,  50,  50,  50,  51,
    51,  51,  224, 25,  224, 301, 302, 51,  51,  51,  51,  224, 22,  22,  51,  51,  51,  51,  51,
    51,  51,  51,  224, 302, 51,  51,  224, 224, 51,  51,  51,  51,  51,  51,  51,  51,  51};

const short ParserGen::yyr1_[] = {
    0,   164, 355, 355, 355, 209, 210, 210, 357, 356, 211, 211, 211, 211, 211, 211, 217, 212, 213,
    223, 223, 223, 223, 214, 215, 216, 218, 218, 177, 177, 220, 221, 221, 221, 221, 221, 221, 221,
    221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221, 221,
    221, 221, 221, 221, 221, 221, 221, 221, 165, 166, 166, 166, 222, 219, 219, 178, 178, 339, 340,
    340, 344, 344, 342, 342, 341, 341, 346, 347, 347, 345, 348, 348, 348, 343, 343, 176, 176, 176,
    172, 168, 168, 168, 168, 168, 168, 169, 170, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181,
    181, 181, 181, 181, 181, 181, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 194, 194, 194, 194, 194,
    194, 194, 194, 194, 194, 195, 208, 196, 197, 198, 200, 201, 202, 182, 183, 184, 185, 187, 191,
    192, 186, 186, 186, 186, 188, 188, 188, 188, 189, 189, 189, 189, 190, 190, 190, 190, 199, 199,
    203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203, 203,
    203, 203, 307, 307, 224, 224, 224, 224, 334, 334, 335, 335, 336, 336, 336, 336, 336, 336, 336,
    336, 336, 336, 226, 227, 225, 225, 228, 229, 229, 230, 337, 338, 338, 231, 232, 232, 179, 167,
    167, 167, 167, 173, 174, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233,
    233, 233, 234, 234, 234, 234, 234, 234, 234, 234, 234, 318, 318, 318, 318, 318, 318, 318, 318,
    318, 318, 318, 318, 318, 318, 318, 235, 331, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295,
    296, 297, 298, 299, 300, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 332, 333,
    236, 236, 236, 237, 238, 239, 243, 243, 243, 243, 243, 243, 243, 243, 243, 243, 243, 243, 243,
    243, 243, 243, 243, 243, 243, 243, 243, 243, 244, 303, 303, 304, 304, 245, 246, 309, 309, 309,
    247, 248, 305, 305, 249, 256, 266, 306, 306, 253, 250, 251, 252, 254, 255, 257, 258, 259, 260,
    261, 262, 263, 264, 265, 353, 353, 351, 349, 350, 350, 352, 352, 352, 352, 352, 352, 352, 352,
    354, 354, 310, 310, 310, 310, 310, 310, 310, 311, 312, 313, 314, 315, 316, 317, 240, 240, 241,
    242, 193, 193, 204, 204, 205, 308, 308, 206, 207, 207, 180, 175, 175, 175, 175, 175, 267, 267,
    267, 267, 267, 267, 267, 268, 269, 270, 271, 272, 273, 274, 275, 275, 275, 275, 275, 275, 275,
    275, 275, 275, 301, 301, 302, 302, 276, 277, 278, 279, 280, 281, 282, 283, 284, 285};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 3, 0, 4, 0, 2, 1,  1,  1,  1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2,  2,  4, 0, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1,  2,  2,  2, 3, 0, 2, 2, 1, 1, 3, 0, 2, 1,  2,  5, 5, 1, 1, 1,
    0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 0, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 4, 5, 1, 1, 1, 4,  4,  3, 3, 1, 1, 3,
    0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  4, 4, 4, 4, 4,
    4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7,  4,  4, 4, 7, 4, 7,
    8, 7, 7, 4, 7, 7, 4, 4, 4, 4, 4,  4,  4,  4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 4,  4,  6, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2, 8, 8,
    0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4,  11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1, 1,  4,  3, 0, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1,  1,  1,  1, 1, 6, 6, 4, 8, 8, 4, 8, 1, 1,  6,  6, 1, 1, 1, 1,
    3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4,  4,  4, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4,  4,  4, 4, 4, 4, 4, 4, 4};


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
                                           "ASIN",
                                           "ASINH",
                                           "ATAN",
                                           "ATAN2",
                                           "ATANH",
                                           "\"false\"",
                                           "\"true\"",
                                           "CEIL",
                                           "CMP",
                                           "CONCAT",
                                           "CONST_EXPR",
                                           "CONVERT",
                                           "COS",
                                           "COSH",
                                           "DATE_FROM_STRING",
                                           "DATE_TO_STRING",
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
                                           "nonArrayNonObjCompoundExpression",
                                           "expressionSingletonArray",
                                           "singleArgExpression",
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
    0,    349,  349,  352,  355,  362,  368,  369,  377,  377,  380,  380,  380,  380,  380,  380,
    383,  393,  399,  409,  409,  409,  409,  413,  418,  423,  442,  445,  452,  455,  461,  475,
    476,  477,  478,  479,  480,  481,  482,  483,  484,  485,  486,  489,  492,  495,  498,  501,
    504,  507,  510,  513,  516,  519,  522,  525,  528,  531,  534,  537,  540,  541,  542,  543,
    544,  549,  557,  570,  571,  588,  595,  599,  607,  610,  616,  622,  625,  631,  634,  643,
    644,  650,  653,  660,  663,  668,  678,  686,  687,  688,  691,  694,  701,  701,  701,  704,
    712,  715,  718,  721,  724,  727,  733,  739,  758,  761,  764,  767,  770,  773,  776,  779,
    782,  785,  788,  791,  794,  797,  800,  803,  811,  814,  817,  820,  823,  826,  829,  832,
    835,  838,  841,  844,  847,  850,  853,  856,  859,  862,  865,  868,  871,  874,  877,  880,
    883,  886,  889,  892,  895,  898,  901,  904,  907,  910,  913,  916,  919,  922,  925,  928,
    931,  934,  937,  940,  943,  946,  949,  952,  955,  958,  961,  964,  967,  970,  973,  976,
    979,  982,  985,  988,  991,  994,  997,  1000, 1003, 1006, 1009, 1012, 1015, 1018, 1021, 1024,
    1027, 1030, 1033, 1036, 1039, 1042, 1045, 1048, 1051, 1054, 1057, 1060, 1067, 1072, 1075, 1078,
    1081, 1084, 1087, 1090, 1093, 1096, 1102, 1116, 1130, 1136, 1142, 1148, 1154, 1160, 1166, 1172,
    1178, 1184, 1190, 1196, 1202, 1208, 1211, 1214, 1217, 1223, 1226, 1229, 1232, 1238, 1241, 1244,
    1247, 1253, 1256, 1259, 1262, 1268, 1271, 1277, 1278, 1279, 1280, 1281, 1282, 1283, 1284, 1285,
    1286, 1287, 1288, 1289, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1304, 1305, 1312, 1312,
    1312, 1312, 1316, 1316, 1320, 1320, 1324, 1324, 1324, 1324, 1324, 1324, 1324, 1325, 1325, 1325,
    1330, 1337, 1343, 1343, 1347, 1351, 1355, 1364, 1371, 1376, 1376, 1381, 1387, 1390, 1397, 1404,
    1404, 1404, 1404, 1408, 1414, 1420, 1420, 1420, 1420, 1420, 1420, 1420, 1420, 1420, 1420, 1420,
    1420, 1421, 1421, 1421, 1421, 1425, 1428, 1431, 1434, 1437, 1440, 1443, 1446, 1449, 1454, 1454,
    1454, 1454, 1454, 1454, 1454, 1454, 1454, 1454, 1454, 1454, 1454, 1455, 1455, 1459, 1466, 1472,
    1477, 1482, 1488, 1493, 1498, 1503, 1509, 1514, 1520, 1529, 1535, 1541, 1546, 1552, 1558, 1563,
    1568, 1573, 1578, 1583, 1588, 1593, 1598, 1603, 1608, 1613, 1618, 1623, 1629, 1629, 1629, 1633,
    1640, 1647, 1654, 1654, 1654, 1654, 1654, 1654, 1654, 1655, 1655, 1655, 1655, 1655, 1655, 1655,
    1655, 1656, 1656, 1656, 1656, 1656, 1656, 1656, 1660, 1670, 1673, 1679, 1682, 1688, 1697, 1706,
    1709, 1712, 1718, 1729, 1740, 1743, 1749, 1757, 1765, 1773, 1776, 1781, 1790, 1796, 1802, 1808,
    1818, 1828, 1835, 1842, 1849, 1857, 1865, 1873, 1881, 1887, 1893, 1896, 1902, 1908, 1913, 1916,
    1923, 1926, 1929, 1932, 1935, 1938, 1941, 1944, 1949, 1951, 1957, 1957, 1957, 1957, 1957, 1957,
    1958, 1962, 1968, 1974, 1981, 1992, 2003, 2010, 2021, 2021, 2025, 2032, 2039, 2039, 2043, 2043,
    2047, 2053, 2054, 2061, 2067, 2070, 2077, 2084, 2085, 2086, 2087, 2088, 2091, 2091, 2091, 2091,
    2091, 2091, 2091, 2093, 2098, 2103, 2108, 2113, 2118, 2123, 2129, 2130, 2131, 2132, 2133, 2134,
    2135, 2136, 2137, 2138, 2143, 2146, 2153, 2156, 2162, 2172, 2177, 2182, 2187, 2192, 2197, 2202,
    2207, 2212};

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
#line 7598 "parser_gen.cpp"

#line 2216 "grammar.yy"
