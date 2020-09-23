// A Bison parser, made by GNU Bison 3.7.1.

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
        case symbol_kind::S_BINARY:  // "BinData"
            value.YY_MOVE_OR_COPY<BSONBinData>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.YY_MOVE_OR_COPY<BSONCode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.YY_MOVE_OR_COPY<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.YY_MOVE_OR_COPY<BSONDBRef>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.YY_MOVE_OR_COPY<BSONRegEx>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.YY_MOVE_OR_COPY<BSONSymbol>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.YY_MOVE_OR_COPY<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.YY_MOVE_OR_COPY<Date_t>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.YY_MOVE_OR_COPY<Decimal128>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.YY_MOVE_OR_COPY<OID>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.YY_MOVE_OR_COPY<Timestamp>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.YY_MOVE_OR_COPY<UserMaxKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.YY_MOVE_OR_COPY<UserMinKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.YY_MOVE_OR_COPY<UserNull>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.YY_MOVE_OR_COPY<UserUndefined>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.YY_MOVE_OR_COPY<double>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.YY_MOVE_OR_COPY<int>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.YY_MOVE_OR_COPY<long long>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
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
        case symbol_kind::S_BINARY:  // "BinData"
            value.move<BSONBinData>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.move<BSONCode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.move<BSONCodeWScope>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.move<BSONDBRef>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.move<BSONRegEx>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.move<BSONSymbol>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.move<CNode::Fieldname>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.move<Date_t>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.move<Decimal128>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.move<OID>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.move<Timestamp>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.move<UserMaxKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.move<UserMinKey>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.move<UserNull>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.move<UserUndefined>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.move<double>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.move<int>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.move<long long>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
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
        case symbol_kind::S_BINARY:  // "BinData"
            value.copy<BSONBinData>(that.value);
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.copy<BSONCode>(that.value);
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.copy<BSONCodeWScope>(that.value);
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.copy<BSONDBRef>(that.value);
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.copy<BSONRegEx>(that.value);
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.copy<BSONSymbol>(that.value);
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.copy<CNode::Fieldname>(that.value);
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.copy<Date_t>(that.value);
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.copy<Decimal128>(that.value);
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.copy<OID>(that.value);
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.copy<Timestamp>(that.value);
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.copy<UserMaxKey>(that.value);
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.copy<UserMinKey>(that.value);
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.copy<UserNull>(that.value);
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.copy<UserUndefined>(that.value);
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.copy<double>(that.value);
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.copy<int>(that.value);
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.copy<long long>(that.value);
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.copy<std::string>(that.value);
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.copy<std::vector<CNode>>(that.value);
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
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
        case symbol_kind::S_BINARY:  // "BinData"
            value.move<BSONBinData>(that.value);
            break;

        case symbol_kind::S_JAVASCRIPT:  // "Code"
            value.move<BSONCode>(that.value);
            break;

        case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
            value.move<BSONCodeWScope>(that.value);
            break;

        case symbol_kind::S_DB_POINTER:  // "dbPointer"
            value.move<BSONDBRef>(that.value);
            break;

        case symbol_kind::S_REGEX:  // "regex"
            value.move<BSONRegEx>(that.value);
            break;

        case symbol_kind::S_SYMBOL:  // "Symbol"
            value.move<BSONSymbol>(that.value);
            break;

        case symbol_kind::S_dbPointer:                          // dbPointer
        case symbol_kind::S_javascript:                         // javascript
        case symbol_kind::S_symbol:                             // symbol
        case symbol_kind::S_javascriptWScope:                   // javascriptWScope
        case symbol_kind::S_int:                                // int
        case symbol_kind::S_timestamp:                          // timestamp
        case symbol_kind::S_long:                               // long
        case symbol_kind::S_double:                             // double
        case symbol_kind::S_decimal:                            // decimal
        case symbol_kind::S_minKey:                             // minKey
        case symbol_kind::S_maxKey:                             // maxKey
        case symbol_kind::S_value:                              // value
        case symbol_kind::S_string:                             // string
        case symbol_kind::S_aggregationFieldPath:               // aggregationFieldPath
        case symbol_kind::S_binary:                             // binary
        case symbol_kind::S_undefined:                          // undefined
        case symbol_kind::S_objectId:                           // objectId
        case symbol_kind::S_bool:                               // bool
        case symbol_kind::S_date:                               // date
        case symbol_kind::S_null:                               // null
        case symbol_kind::S_regex:                              // regex
        case symbol_kind::S_simpleValue:                        // simpleValue
        case symbol_kind::S_compoundValue:                      // compoundValue
        case symbol_kind::S_valueArray:                         // valueArray
        case symbol_kind::S_valueObject:                        // valueObject
        case symbol_kind::S_valueFields:                        // valueFields
        case symbol_kind::S_variable:                           // variable
        case symbol_kind::S_typeArray:                          // typeArray
        case symbol_kind::S_typeValue:                          // typeValue
        case symbol_kind::S_pipeline:                           // pipeline
        case symbol_kind::S_stageList:                          // stageList
        case symbol_kind::S_stage:                              // stage
        case symbol_kind::S_inhibitOptimization:                // inhibitOptimization
        case symbol_kind::S_unionWith:                          // unionWith
        case symbol_kind::S_skip:                               // skip
        case symbol_kind::S_limit:                              // limit
        case symbol_kind::S_matchStage:                         // matchStage
        case symbol_kind::S_project:                            // project
        case symbol_kind::S_sample:                             // sample
        case symbol_kind::S_aggregationProjectFields:           // aggregationProjectFields
        case symbol_kind::S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
        case symbol_kind::S_topLevelAggregationProjection:      // topLevelAggregationProjection
        case symbol_kind::S_aggregationProjection:              // aggregationProjection
        case symbol_kind::S_projectionCommon:                   // projectionCommon
        case symbol_kind::S_aggregationProjectionObject:        // aggregationProjectionObject
        case symbol_kind::S_num:                                // num
        case symbol_kind::S_expression:                         // expression
        case symbol_kind::S_exprFixedTwoArg:                    // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                  // exprFixedThreeArg
        case symbol_kind::S_slice:                              // slice
        case symbol_kind::S_expressionArray:                    // expressionArray
        case symbol_kind::S_expressionObject:                   // expressionObject
        case symbol_kind::S_expressionFields:                   // expressionFields
        case symbol_kind::S_maths:                              // maths
        case symbol_kind::S_meta:                               // meta
        case symbol_kind::S_add:                                // add
        case symbol_kind::S_boolExprs:                          // boolExprs
        case symbol_kind::S_and:                                // and
        case symbol_kind::S_or:                                 // or
        case symbol_kind::S_not:                                // not
        case symbol_kind::S_literalEscapes:                     // literalEscapes
        case symbol_kind::S_const:                              // const
        case symbol_kind::S_literal:                            // literal
        case symbol_kind::S_stringExps:                         // stringExps
        case symbol_kind::S_concat:                             // concat
        case symbol_kind::S_dateFromString:                     // dateFromString
        case symbol_kind::S_dateToString:                       // dateToString
        case symbol_kind::S_indexOfBytes:                       // indexOfBytes
        case symbol_kind::S_indexOfCP:                          // indexOfCP
        case symbol_kind::S_ltrim:                              // ltrim
        case symbol_kind::S_regexFind:                          // regexFind
        case symbol_kind::S_regexFindAll:                       // regexFindAll
        case symbol_kind::S_regexMatch:                         // regexMatch
        case symbol_kind::S_regexArgs:                          // regexArgs
        case symbol_kind::S_replaceOne:                         // replaceOne
        case symbol_kind::S_replaceAll:                         // replaceAll
        case symbol_kind::S_rtrim:                              // rtrim
        case symbol_kind::S_split:                              // split
        case symbol_kind::S_strLenBytes:                        // strLenBytes
        case symbol_kind::S_strLenCP:                           // strLenCP
        case symbol_kind::S_strcasecmp:                         // strcasecmp
        case symbol_kind::S_substr:                             // substr
        case symbol_kind::S_substrBytes:                        // substrBytes
        case symbol_kind::S_substrCP:                           // substrCP
        case symbol_kind::S_toLower:                            // toLower
        case symbol_kind::S_toUpper:                            // toUpper
        case symbol_kind::S_trim:                               // trim
        case symbol_kind::S_compExprs:                          // compExprs
        case symbol_kind::S_cmp:                                // cmp
        case symbol_kind::S_eq:                                 // eq
        case symbol_kind::S_gt:                                 // gt
        case symbol_kind::S_gte:                                // gte
        case symbol_kind::S_lt:                                 // lt
        case symbol_kind::S_lte:                                // lte
        case symbol_kind::S_ne:                                 // ne
        case symbol_kind::S_dateExps:                           // dateExps
        case symbol_kind::S_dateFromParts:                      // dateFromParts
        case symbol_kind::S_dateToParts:                        // dateToParts
        case symbol_kind::S_dayOfMonth:                         // dayOfMonth
        case symbol_kind::S_dayOfWeek:                          // dayOfWeek
        case symbol_kind::S_dayOfYear:                          // dayOfYear
        case symbol_kind::S_hour:                               // hour
        case symbol_kind::S_isoDayOfWeek:                       // isoDayOfWeek
        case symbol_kind::S_isoWeek:                            // isoWeek
        case symbol_kind::S_isoWeekYear:                        // isoWeekYear
        case symbol_kind::S_millisecond:                        // millisecond
        case symbol_kind::S_minute:                             // minute
        case symbol_kind::S_month:                              // month
        case symbol_kind::S_second:                             // second
        case symbol_kind::S_week:                               // week
        case symbol_kind::S_year:                               // year
        case symbol_kind::S_typeExpression:                     // typeExpression
        case symbol_kind::S_convert:                            // convert
        case symbol_kind::S_toBool:                             // toBool
        case symbol_kind::S_toDate:                             // toDate
        case symbol_kind::S_toDecimal:                          // toDecimal
        case symbol_kind::S_toDouble:                           // toDouble
        case symbol_kind::S_toInt:                              // toInt
        case symbol_kind::S_toLong:                             // toLong
        case symbol_kind::S_toObjectId:                         // toObjectId
        case symbol_kind::S_toString:                           // toString
        case symbol_kind::S_type:                               // type
        case symbol_kind::S_abs:                                // abs
        case symbol_kind::S_ceil:                               // ceil
        case symbol_kind::S_divide:                             // divide
        case symbol_kind::S_exponent:                           // exponent
        case symbol_kind::S_floor:                              // floor
        case symbol_kind::S_ln:                                 // ln
        case symbol_kind::S_log:                                // log
        case symbol_kind::S_logten:                             // logten
        case symbol_kind::S_mod:                                // mod
        case symbol_kind::S_multiply:                           // multiply
        case symbol_kind::S_pow:                                // pow
        case symbol_kind::S_round:                              // round
        case symbol_kind::S_sqrt:                               // sqrt
        case symbol_kind::S_subtract:                           // subtract
        case symbol_kind::S_trunc:                              // trunc
        case symbol_kind::S_arrayExps:                          // arrayExps
        case symbol_kind::S_arrayElemAt:                        // arrayElemAt
        case symbol_kind::S_arrayToObject:                      // arrayToObject
        case symbol_kind::S_concatArrays:                       // concatArrays
        case symbol_kind::S_filter:                             // filter
        case symbol_kind::S_first:                              // first
        case symbol_kind::S_in:                                 // in
        case symbol_kind::S_indexOfArray:                       // indexOfArray
        case symbol_kind::S_isArray:                            // isArray
        case symbol_kind::S_setExpression:                      // setExpression
        case symbol_kind::S_allElementsTrue:                    // allElementsTrue
        case symbol_kind::S_anyElementTrue:                     // anyElementTrue
        case symbol_kind::S_setDifference:                      // setDifference
        case symbol_kind::S_setEquals:                          // setEquals
        case symbol_kind::S_setIntersection:                    // setIntersection
        case symbol_kind::S_setIsSubset:                        // setIsSubset
        case symbol_kind::S_setUnion:                           // setUnion
        case symbol_kind::S_trig:                               // trig
        case symbol_kind::S_sin:                                // sin
        case symbol_kind::S_cos:                                // cos
        case symbol_kind::S_tan:                                // tan
        case symbol_kind::S_sinh:                               // sinh
        case symbol_kind::S_cosh:                               // cosh
        case symbol_kind::S_tanh:                               // tanh
        case symbol_kind::S_asin:                               // asin
        case symbol_kind::S_acos:                               // acos
        case symbol_kind::S_atan:                               // atan
        case symbol_kind::S_asinh:                              // asinh
        case symbol_kind::S_acosh:                              // acosh
        case symbol_kind::S_atanh:                              // atanh
        case symbol_kind::S_atan2:                              // atan2
        case symbol_kind::S_degreesToRadians:                   // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                   // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                 // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:         // nonArrayCompoundExpression
        case symbol_kind::S_aggregationOperator:                // aggregationOperator
        case symbol_kind::S_aggregationOperatorWithoutSlice:    // aggregationOperatorWithoutSlice
        case symbol_kind::S_expressionSingletonArray:           // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:                // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:           // nonArrayNonObjExpression
        case symbol_kind::S_matchExpression:                    // matchExpression
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
        case symbol_kind::S_textArgCaseSensitive:               // textArgCaseSensitive
        case symbol_kind::S_textArgDiacriticSensitive:          // textArgDiacriticSensitive
        case symbol_kind::S_textArgLanguage:                    // textArgLanguage
        case symbol_kind::S_textArgSearch:                      // textArgSearch
        case symbol_kind::S_findProject:                        // findProject
        case symbol_kind::S_findProjectFields:                  // findProjectFields
        case symbol_kind::S_topLevelFindProjection:             // topLevelFindProjection
        case symbol_kind::S_findProjection:                     // findProjection
        case symbol_kind::S_findProjectionSlice:                // findProjectionSlice
        case symbol_kind::S_elemMatch:                          // elemMatch
        case symbol_kind::S_findProjectionObject:               // findProjectionObject
        case symbol_kind::S_findProjectionObjectFields:         // findProjectionObjectFields
        case symbol_kind::S_sortSpecs:                          // sortSpecs
        case symbol_kind::S_specList:                           // specList
        case symbol_kind::S_metaSort:                           // metaSort
        case symbol_kind::S_oneOrNegOne:                        // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                    // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case symbol_kind::S_aggregationProjectionFieldname:  // aggregationProjectionFieldname
        case symbol_kind::S_projectionFieldname:             // projectionFieldname
        case symbol_kind::S_expressionFieldname:             // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:            // stageAsUserFieldname
        case symbol_kind::S_argAsUserFieldname:              // argAsUserFieldname
        case symbol_kind::S_argAsProjectionPath:             // argAsProjectionPath
        case symbol_kind::S_aggExprAsUserFieldname:          // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:         // invariableUserFieldname
        case symbol_kind::S_sortFieldname:                   // sortFieldname
        case symbol_kind::S_idAsUserFieldname:               // idAsUserFieldname
        case symbol_kind::S_elemMatchAsUserFieldname:        // elemMatchAsUserFieldname
        case symbol_kind::S_idAsProjectionPath:              // idAsProjectionPath
        case symbol_kind::S_valueFieldname:                  // valueFieldname
        case symbol_kind::S_predFieldname:                   // predFieldname
        case symbol_kind::S_logicalExprField:                // logicalExprField
            value.move<CNode::Fieldname>(that.value);
            break;

        case symbol_kind::S_DATE_LITERAL:  // "Date"
            value.move<Date_t>(that.value);
            break;

        case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
            value.move<Decimal128>(that.value);
            break;

        case symbol_kind::S_OBJECT_ID:  // "ObjectID"
            value.move<OID>(that.value);
            break;

        case symbol_kind::S_TIMESTAMP:  // "Timestamp"
            value.move<Timestamp>(that.value);
            break;

        case symbol_kind::S_MAX_KEY:  // "maxKey"
            value.move<UserMaxKey>(that.value);
            break;

        case symbol_kind::S_MIN_KEY:  // "minKey"
            value.move<UserMinKey>(that.value);
            break;

        case symbol_kind::S_JSNULL:  // "null"
            value.move<UserNull>(that.value);
            break;

        case symbol_kind::S_UNDEFINED:  // "undefined"
            value.move<UserUndefined>(that.value);
            break;

        case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
            value.move<double>(that.value);
            break;

        case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
            value.move<int>(that.value);
            break;

        case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
            value.move<long long>(that.value);
            break;

        case symbol_kind::S_aggregationProjectField:           // aggregationProjectField
        case symbol_kind::S_aggregationProjectionObjectField:  // aggregationProjectionObjectField
        case symbol_kind::S_expressionField:                   // expressionField
        case symbol_kind::S_valueField:                        // valueField
        case symbol_kind::S_onErrorArg:                        // onErrorArg
        case symbol_kind::S_onNullArg:                         // onNullArg
        case symbol_kind::S_asArg:                             // asArg
        case symbol_kind::S_formatArg:                         // formatArg
        case symbol_kind::S_timezoneArg:                       // timezoneArg
        case symbol_kind::S_charsArg:                          // charsArg
        case symbol_kind::S_optionsArg:                        // optionsArg
        case symbol_kind::S_hourArg:                           // hourArg
        case symbol_kind::S_minuteArg:                         // minuteArg
        case symbol_kind::S_secondArg:                         // secondArg
        case symbol_kind::S_millisecondArg:                    // millisecondArg
        case symbol_kind::S_dayArg:                            // dayArg
        case symbol_kind::S_isoWeekArg:                        // isoWeekArg
        case symbol_kind::S_iso8601Arg:                        // iso8601Arg
        case symbol_kind::S_monthArg:                          // monthArg
        case symbol_kind::S_isoDayOfWeekArg:                   // isoDayOfWeekArg
        case symbol_kind::S_predicate:                         // predicate
        case symbol_kind::S_fieldPredicate:                    // fieldPredicate
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_matchMod:                          // matchMod
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
        case symbol_kind::S_matchExpr:                         // matchExpr
        case symbol_kind::S_matchText:                         // matchText
        case symbol_kind::S_matchWhere:                        // matchWhere
        case symbol_kind::S_findProjectField:                  // findProjectField
        case symbol_kind::S_findProjectionObjectField:         // findProjectionObjectField
        case symbol_kind::S_sortSpec:                          // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_arg:                    // arg
            value.move<std::string>(that.value);
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
        case symbol_kind::S_typeValues:     // typeValues
            value.move<std::vector<CNode>>(that.value);
            break;

        case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
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
                case symbol_kind::S_BINARY:  // "BinData"
                    yylhs.value.emplace<BSONBinData>();
                    break;

                case symbol_kind::S_JAVASCRIPT:  // "Code"
                    yylhs.value.emplace<BSONCode>();
                    break;

                case symbol_kind::S_JAVASCRIPT_W_SCOPE:  // "CodeWScope"
                    yylhs.value.emplace<BSONCodeWScope>();
                    break;

                case symbol_kind::S_DB_POINTER:  // "dbPointer"
                    yylhs.value.emplace<BSONDBRef>();
                    break;

                case symbol_kind::S_REGEX:  // "regex"
                    yylhs.value.emplace<BSONRegEx>();
                    break;

                case symbol_kind::S_SYMBOL:  // "Symbol"
                    yylhs.value.emplace<BSONSymbol>();
                    break;

                case symbol_kind::S_dbPointer:                 // dbPointer
                case symbol_kind::S_javascript:                // javascript
                case symbol_kind::S_symbol:                    // symbol
                case symbol_kind::S_javascriptWScope:          // javascriptWScope
                case symbol_kind::S_int:                       // int
                case symbol_kind::S_timestamp:                 // timestamp
                case symbol_kind::S_long:                      // long
                case symbol_kind::S_double:                    // double
                case symbol_kind::S_decimal:                   // decimal
                case symbol_kind::S_minKey:                    // minKey
                case symbol_kind::S_maxKey:                    // maxKey
                case symbol_kind::S_value:                     // value
                case symbol_kind::S_string:                    // string
                case symbol_kind::S_aggregationFieldPath:      // aggregationFieldPath
                case symbol_kind::S_binary:                    // binary
                case symbol_kind::S_undefined:                 // undefined
                case symbol_kind::S_objectId:                  // objectId
                case symbol_kind::S_bool:                      // bool
                case symbol_kind::S_date:                      // date
                case symbol_kind::S_null:                      // null
                case symbol_kind::S_regex:                     // regex
                case symbol_kind::S_simpleValue:               // simpleValue
                case symbol_kind::S_compoundValue:             // compoundValue
                case symbol_kind::S_valueArray:                // valueArray
                case symbol_kind::S_valueObject:               // valueObject
                case symbol_kind::S_valueFields:               // valueFields
                case symbol_kind::S_variable:                  // variable
                case symbol_kind::S_typeArray:                 // typeArray
                case symbol_kind::S_typeValue:                 // typeValue
                case symbol_kind::S_pipeline:                  // pipeline
                case symbol_kind::S_stageList:                 // stageList
                case symbol_kind::S_stage:                     // stage
                case symbol_kind::S_inhibitOptimization:       // inhibitOptimization
                case symbol_kind::S_unionWith:                 // unionWith
                case symbol_kind::S_skip:                      // skip
                case symbol_kind::S_limit:                     // limit
                case symbol_kind::S_matchStage:                // matchStage
                case symbol_kind::S_project:                   // project
                case symbol_kind::S_sample:                    // sample
                case symbol_kind::S_aggregationProjectFields:  // aggregationProjectFields
                case symbol_kind::
                    S_aggregationProjectionObjectFields:  // aggregationProjectionObjectFields
                case symbol_kind::S_topLevelAggregationProjection:  // topLevelAggregationProjection
                case symbol_kind::S_aggregationProjection:          // aggregationProjection
                case symbol_kind::S_projectionCommon:               // projectionCommon
                case symbol_kind::S_aggregationProjectionObject:    // aggregationProjectionObject
                case symbol_kind::S_num:                            // num
                case symbol_kind::S_expression:                     // expression
                case symbol_kind::S_exprFixedTwoArg:                // exprFixedTwoArg
                case symbol_kind::S_exprFixedThreeArg:              // exprFixedThreeArg
                case symbol_kind::S_slice:                          // slice
                case symbol_kind::S_expressionArray:                // expressionArray
                case symbol_kind::S_expressionObject:               // expressionObject
                case symbol_kind::S_expressionFields:               // expressionFields
                case symbol_kind::S_maths:                          // maths
                case symbol_kind::S_meta:                           // meta
                case symbol_kind::S_add:                            // add
                case symbol_kind::S_boolExprs:                      // boolExprs
                case symbol_kind::S_and:                            // and
                case symbol_kind::S_or:                             // or
                case symbol_kind::S_not:                            // not
                case symbol_kind::S_literalEscapes:                 // literalEscapes
                case symbol_kind::S_const:                          // const
                case symbol_kind::S_literal:                        // literal
                case symbol_kind::S_stringExps:                     // stringExps
                case symbol_kind::S_concat:                         // concat
                case symbol_kind::S_dateFromString:                 // dateFromString
                case symbol_kind::S_dateToString:                   // dateToString
                case symbol_kind::S_indexOfBytes:                   // indexOfBytes
                case symbol_kind::S_indexOfCP:                      // indexOfCP
                case symbol_kind::S_ltrim:                          // ltrim
                case symbol_kind::S_regexFind:                      // regexFind
                case symbol_kind::S_regexFindAll:                   // regexFindAll
                case symbol_kind::S_regexMatch:                     // regexMatch
                case symbol_kind::S_regexArgs:                      // regexArgs
                case symbol_kind::S_replaceOne:                     // replaceOne
                case symbol_kind::S_replaceAll:                     // replaceAll
                case symbol_kind::S_rtrim:                          // rtrim
                case symbol_kind::S_split:                          // split
                case symbol_kind::S_strLenBytes:                    // strLenBytes
                case symbol_kind::S_strLenCP:                       // strLenCP
                case symbol_kind::S_strcasecmp:                     // strcasecmp
                case symbol_kind::S_substr:                         // substr
                case symbol_kind::S_substrBytes:                    // substrBytes
                case symbol_kind::S_substrCP:                       // substrCP
                case symbol_kind::S_toLower:                        // toLower
                case symbol_kind::S_toUpper:                        // toUpper
                case symbol_kind::S_trim:                           // trim
                case symbol_kind::S_compExprs:                      // compExprs
                case symbol_kind::S_cmp:                            // cmp
                case symbol_kind::S_eq:                             // eq
                case symbol_kind::S_gt:                             // gt
                case symbol_kind::S_gte:                            // gte
                case symbol_kind::S_lt:                             // lt
                case symbol_kind::S_lte:                            // lte
                case symbol_kind::S_ne:                             // ne
                case symbol_kind::S_dateExps:                       // dateExps
                case symbol_kind::S_dateFromParts:                  // dateFromParts
                case symbol_kind::S_dateToParts:                    // dateToParts
                case symbol_kind::S_dayOfMonth:                     // dayOfMonth
                case symbol_kind::S_dayOfWeek:                      // dayOfWeek
                case symbol_kind::S_dayOfYear:                      // dayOfYear
                case symbol_kind::S_hour:                           // hour
                case symbol_kind::S_isoDayOfWeek:                   // isoDayOfWeek
                case symbol_kind::S_isoWeek:                        // isoWeek
                case symbol_kind::S_isoWeekYear:                    // isoWeekYear
                case symbol_kind::S_millisecond:                    // millisecond
                case symbol_kind::S_minute:                         // minute
                case symbol_kind::S_month:                          // month
                case symbol_kind::S_second:                         // second
                case symbol_kind::S_week:                           // week
                case symbol_kind::S_year:                           // year
                case symbol_kind::S_typeExpression:                 // typeExpression
                case symbol_kind::S_convert:                        // convert
                case symbol_kind::S_toBool:                         // toBool
                case symbol_kind::S_toDate:                         // toDate
                case symbol_kind::S_toDecimal:                      // toDecimal
                case symbol_kind::S_toDouble:                       // toDouble
                case symbol_kind::S_toInt:                          // toInt
                case symbol_kind::S_toLong:                         // toLong
                case symbol_kind::S_toObjectId:                     // toObjectId
                case symbol_kind::S_toString:                       // toString
                case symbol_kind::S_type:                           // type
                case symbol_kind::S_abs:                            // abs
                case symbol_kind::S_ceil:                           // ceil
                case symbol_kind::S_divide:                         // divide
                case symbol_kind::S_exponent:                       // exponent
                case symbol_kind::S_floor:                          // floor
                case symbol_kind::S_ln:                             // ln
                case symbol_kind::S_log:                            // log
                case symbol_kind::S_logten:                         // logten
                case symbol_kind::S_mod:                            // mod
                case symbol_kind::S_multiply:                       // multiply
                case symbol_kind::S_pow:                            // pow
                case symbol_kind::S_round:                          // round
                case symbol_kind::S_sqrt:                           // sqrt
                case symbol_kind::S_subtract:                       // subtract
                case symbol_kind::S_trunc:                          // trunc
                case symbol_kind::S_arrayExps:                      // arrayExps
                case symbol_kind::S_arrayElemAt:                    // arrayElemAt
                case symbol_kind::S_arrayToObject:                  // arrayToObject
                case symbol_kind::S_concatArrays:                   // concatArrays
                case symbol_kind::S_filter:                         // filter
                case symbol_kind::S_first:                          // first
                case symbol_kind::S_in:                             // in
                case symbol_kind::S_indexOfArray:                   // indexOfArray
                case symbol_kind::S_isArray:                        // isArray
                case symbol_kind::S_setExpression:                  // setExpression
                case symbol_kind::S_allElementsTrue:                // allElementsTrue
                case symbol_kind::S_anyElementTrue:                 // anyElementTrue
                case symbol_kind::S_setDifference:                  // setDifference
                case symbol_kind::S_setEquals:                      // setEquals
                case symbol_kind::S_setIntersection:                // setIntersection
                case symbol_kind::S_setIsSubset:                    // setIsSubset
                case symbol_kind::S_setUnion:                       // setUnion
                case symbol_kind::S_trig:                           // trig
                case symbol_kind::S_sin:                            // sin
                case symbol_kind::S_cos:                            // cos
                case symbol_kind::S_tan:                            // tan
                case symbol_kind::S_sinh:                           // sinh
                case symbol_kind::S_cosh:                           // cosh
                case symbol_kind::S_tanh:                           // tanh
                case symbol_kind::S_asin:                           // asin
                case symbol_kind::S_acos:                           // acos
                case symbol_kind::S_atan:                           // atan
                case symbol_kind::S_asinh:                          // asinh
                case symbol_kind::S_acosh:                          // acosh
                case symbol_kind::S_atanh:                          // atanh
                case symbol_kind::S_atan2:                          // atan2
                case symbol_kind::S_degreesToRadians:               // degreesToRadians
                case symbol_kind::S_radiansToDegrees:               // radiansToDegrees
                case symbol_kind::S_nonArrayExpression:             // nonArrayExpression
                case symbol_kind::S_nonArrayCompoundExpression:     // nonArrayCompoundExpression
                case symbol_kind::S_aggregationOperator:            // aggregationOperator
                case symbol_kind::
                    S_aggregationOperatorWithoutSlice:           // aggregationOperatorWithoutSlice
                case symbol_kind::S_expressionSingletonArray:    // expressionSingletonArray
                case symbol_kind::S_singleArgExpression:         // singleArgExpression
                case symbol_kind::S_nonArrayNonObjExpression:    // nonArrayNonObjExpression
                case symbol_kind::S_matchExpression:             // matchExpression
                case symbol_kind::S_predicates:                  // predicates
                case symbol_kind::S_compoundMatchExprs:          // compoundMatchExprs
                case symbol_kind::S_predValue:                   // predValue
                case symbol_kind::S_additionalExprs:             // additionalExprs
                case symbol_kind::S_textArgCaseSensitive:        // textArgCaseSensitive
                case symbol_kind::S_textArgDiacriticSensitive:   // textArgDiacriticSensitive
                case symbol_kind::S_textArgLanguage:             // textArgLanguage
                case symbol_kind::S_textArgSearch:               // textArgSearch
                case symbol_kind::S_findProject:                 // findProject
                case symbol_kind::S_findProjectFields:           // findProjectFields
                case symbol_kind::S_topLevelFindProjection:      // topLevelFindProjection
                case symbol_kind::S_findProjection:              // findProjection
                case symbol_kind::S_findProjectionSlice:         // findProjectionSlice
                case symbol_kind::S_elemMatch:                   // elemMatch
                case symbol_kind::S_findProjectionObject:        // findProjectionObject
                case symbol_kind::S_findProjectionObjectFields:  // findProjectionObjectFields
                case symbol_kind::S_sortSpecs:                   // sortSpecs
                case symbol_kind::S_specList:                    // specList
                case symbol_kind::S_metaSort:                    // metaSort
                case symbol_kind::S_oneOrNegOne:                 // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:             // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case symbol_kind::
                    S_aggregationProjectionFieldname:          // aggregationProjectionFieldname
                case symbol_kind::S_projectionFieldname:       // projectionFieldname
                case symbol_kind::S_expressionFieldname:       // expressionFieldname
                case symbol_kind::S_stageAsUserFieldname:      // stageAsUserFieldname
                case symbol_kind::S_argAsUserFieldname:        // argAsUserFieldname
                case symbol_kind::S_argAsProjectionPath:       // argAsProjectionPath
                case symbol_kind::S_aggExprAsUserFieldname:    // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:   // invariableUserFieldname
                case symbol_kind::S_sortFieldname:             // sortFieldname
                case symbol_kind::S_idAsUserFieldname:         // idAsUserFieldname
                case symbol_kind::S_elemMatchAsUserFieldname:  // elemMatchAsUserFieldname
                case symbol_kind::S_idAsProjectionPath:        // idAsProjectionPath
                case symbol_kind::S_valueFieldname:            // valueFieldname
                case symbol_kind::S_predFieldname:             // predFieldname
                case symbol_kind::S_logicalExprField:          // logicalExprField
                    yylhs.value.emplace<CNode::Fieldname>();
                    break;

                case symbol_kind::S_DATE_LITERAL:  // "Date"
                    yylhs.value.emplace<Date_t>();
                    break;

                case symbol_kind::S_DECIMAL_OTHER:  // "arbitrary decimal"
                    yylhs.value.emplace<Decimal128>();
                    break;

                case symbol_kind::S_OBJECT_ID:  // "ObjectID"
                    yylhs.value.emplace<OID>();
                    break;

                case symbol_kind::S_TIMESTAMP:  // "Timestamp"
                    yylhs.value.emplace<Timestamp>();
                    break;

                case symbol_kind::S_MAX_KEY:  // "maxKey"
                    yylhs.value.emplace<UserMaxKey>();
                    break;

                case symbol_kind::S_MIN_KEY:  // "minKey"
                    yylhs.value.emplace<UserMinKey>();
                    break;

                case symbol_kind::S_JSNULL:  // "null"
                    yylhs.value.emplace<UserNull>();
                    break;

                case symbol_kind::S_UNDEFINED:  // "undefined"
                    yylhs.value.emplace<UserUndefined>();
                    break;

                case symbol_kind::S_DOUBLE_OTHER:  // "arbitrary double"
                    yylhs.value.emplace<double>();
                    break;

                case symbol_kind::S_INT_OTHER:  // "arbitrary integer"
                    yylhs.value.emplace<int>();
                    break;

                case symbol_kind::S_LONG_OTHER:  // "arbitrary long"
                    yylhs.value.emplace<long long>();
                    break;

                case symbol_kind::S_aggregationProjectField:  // aggregationProjectField
                case symbol_kind::
                    S_aggregationProjectionObjectField:         // aggregationProjectionObjectField
                case symbol_kind::S_expressionField:            // expressionField
                case symbol_kind::S_valueField:                 // valueField
                case symbol_kind::S_onErrorArg:                 // onErrorArg
                case symbol_kind::S_onNullArg:                  // onNullArg
                case symbol_kind::S_asArg:                      // asArg
                case symbol_kind::S_formatArg:                  // formatArg
                case symbol_kind::S_timezoneArg:                // timezoneArg
                case symbol_kind::S_charsArg:                   // charsArg
                case symbol_kind::S_optionsArg:                 // optionsArg
                case symbol_kind::S_hourArg:                    // hourArg
                case symbol_kind::S_minuteArg:                  // minuteArg
                case symbol_kind::S_secondArg:                  // secondArg
                case symbol_kind::S_millisecondArg:             // millisecondArg
                case symbol_kind::S_dayArg:                     // dayArg
                case symbol_kind::S_isoWeekArg:                 // isoWeekArg
                case symbol_kind::S_iso8601Arg:                 // iso8601Arg
                case symbol_kind::S_monthArg:                   // monthArg
                case symbol_kind::S_isoDayOfWeekArg:            // isoDayOfWeekArg
                case symbol_kind::S_predicate:                  // predicate
                case symbol_kind::S_fieldPredicate:             // fieldPredicate
                case symbol_kind::S_logicalExpr:                // logicalExpr
                case symbol_kind::S_operatorExpression:         // operatorExpression
                case symbol_kind::S_notExpr:                    // notExpr
                case symbol_kind::S_matchMod:                   // matchMod
                case symbol_kind::S_existsExpr:                 // existsExpr
                case symbol_kind::S_typeExpr:                   // typeExpr
                case symbol_kind::S_commentExpr:                // commentExpr
                case symbol_kind::S_matchExpr:                  // matchExpr
                case symbol_kind::S_matchText:                  // matchText
                case symbol_kind::S_matchWhere:                 // matchWhere
                case symbol_kind::S_findProjectField:           // findProjectField
                case symbol_kind::S_findProjectionObjectField:  // findProjectionObjectField
                case symbol_kind::S_sortSpec:                   // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case symbol_kind::S_FIELDNAME:              // "fieldname"
                case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
                case symbol_kind::S_STRING:                 // "string"
                case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
                case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
                case symbol_kind::S_arg:                    // arg
                    yylhs.value.emplace<std::string>();
                    break;

                case symbol_kind::S_expressions:    // expressions
                case symbol_kind::S_values:         // values
                case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
                case symbol_kind::S_typeValues:     // typeValues
                    yylhs.value.emplace<std::vector<CNode>>();
                    break;

                case symbol_kind::S_DOTTED_FIELDNAME:  // "fieldname containing dotted path"
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
                    case 2:  // start: START_PIPELINE pipeline
#line 417 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2293 "parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH matchExpression
#line 420 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2301 "parser_gen.cpp"
                    break;

                    case 4:  // start: START_PROJECT findProject
#line 423 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2309 "parser_gen.cpp"
                    break;

                    case 5:  // start: START_SORT sortSpecs
#line 426 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2317 "parser_gen.cpp"
                    break;

                    case 6:  // pipeline: "array" stageList "end of array"
#line 433 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2325 "parser_gen.cpp"
                    break;

                    case 7:  // stageList: %empty
#line 439 "grammar.yy"
                    {
                    }
#line 2331 "parser_gen.cpp"
                    break;

                    case 8:  // stageList: "object" stage "end of object" stageList
#line 440 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2339 "parser_gen.cpp"
                    break;

                    case 9:  // START_ORDERED_OBJECT: "object"
#line 448 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2345 "parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2351 "parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2357 "parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2363 "parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2369 "parser_gen.cpp"
                    break;

                    case 14:  // stage: matchStage
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2375 "parser_gen.cpp"
                    break;

                    case 15:  // stage: project
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2381 "parser_gen.cpp"
                    break;

                    case 16:  // stage: sample
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2387 "parser_gen.cpp"
                    break;

                    case 17:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2399 "parser_gen.cpp"
                    break;

                    case 18:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 464 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2407 "parser_gen.cpp"
                    break;

                    case 19:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 470 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2420 "parser_gen.cpp"
                    break;

                    case 20:  // num: int
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2426 "parser_gen.cpp"
                    break;

                    case 21:  // num: long
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2432 "parser_gen.cpp"
                    break;

                    case 22:  // num: double
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2438 "parser_gen.cpp"
                    break;

                    case 23:  // num: decimal
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2444 "parser_gen.cpp"
                    break;

                    case 24:  // skip: STAGE_SKIP num
#line 484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2452 "parser_gen.cpp"
                    break;

                    case 25:  // limit: STAGE_LIMIT num
#line 489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2460 "parser_gen.cpp"
                    break;

                    case 26:  // matchStage: STAGE_MATCH matchExpression
#line 494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::match, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2468 "parser_gen.cpp"
                    break;

                    case 27:  // project: STAGE_PROJECT "object" aggregationProjectFields "end of
                              // object"
#line 500 "grammar.yy"
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
#line 2489 "parser_gen.cpp"
                    break;

                    case 28:  // aggregationProjectFields: %empty
#line 519 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2497 "parser_gen.cpp"
                    break;

                    case 29:  // aggregationProjectFields: aggregationProjectFields
                              // aggregationProjectField
#line 522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2506 "parser_gen.cpp"
                    break;

                    case 30:  // aggregationProjectField: ID topLevelAggregationProjection
#line 529 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2514 "parser_gen.cpp"
                    break;

                    case 31:  // aggregationProjectField: aggregationProjectionFieldname
                              // topLevelAggregationProjection
#line 532 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2522 "parser_gen.cpp"
                    break;

                    case 32:  // topLevelAggregationProjection: aggregationProjection
#line 538 "grammar.yy"
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
#line 2538 "parser_gen.cpp"
                    break;

                    case 33:  // aggregationProjection: projectionCommon
#line 552 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2544 "parser_gen.cpp"
                    break;

                    case 34:  // aggregationProjection: aggregationProjectionObject
#line 553 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2550 "parser_gen.cpp"
                    break;

                    case 35:  // aggregationProjection: aggregationOperator
#line 554 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2556 "parser_gen.cpp"
                    break;

                    case 36:  // projectionCommon: string
#line 558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2562 "parser_gen.cpp"
                    break;

                    case 37:  // projectionCommon: binary
#line 559 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2568 "parser_gen.cpp"
                    break;

                    case 38:  // projectionCommon: undefined
#line 560 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2574 "parser_gen.cpp"
                    break;

                    case 39:  // projectionCommon: objectId
#line 561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2580 "parser_gen.cpp"
                    break;

                    case 40:  // projectionCommon: date
#line 562 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2586 "parser_gen.cpp"
                    break;

                    case 41:  // projectionCommon: null
#line 563 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2592 "parser_gen.cpp"
                    break;

                    case 42:  // projectionCommon: regex
#line 564 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2598 "parser_gen.cpp"
                    break;

                    case 43:  // projectionCommon: dbPointer
#line 565 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2604 "parser_gen.cpp"
                    break;

                    case 44:  // projectionCommon: javascript
#line 566 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2610 "parser_gen.cpp"
                    break;

                    case 45:  // projectionCommon: symbol
#line 567 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2616 "parser_gen.cpp"
                    break;

                    case 46:  // projectionCommon: javascriptWScope
#line 568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2622 "parser_gen.cpp"
                    break;

                    case 47:  // projectionCommon: "1 (int)"
#line 569 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2630 "parser_gen.cpp"
                    break;

                    case 48:  // projectionCommon: "-1 (int)"
#line 572 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2638 "parser_gen.cpp"
                    break;

                    case 49:  // projectionCommon: "arbitrary integer"
#line 575 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2646 "parser_gen.cpp"
                    break;

                    case 50:  // projectionCommon: "zero (int)"
#line 578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2654 "parser_gen.cpp"
                    break;

                    case 51:  // projectionCommon: "1 (long)"
#line 581 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2662 "parser_gen.cpp"
                    break;

                    case 52:  // projectionCommon: "-1 (long)"
#line 584 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2670 "parser_gen.cpp"
                    break;

                    case 53:  // projectionCommon: "arbitrary long"
#line 587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2678 "parser_gen.cpp"
                    break;

                    case 54:  // projectionCommon: "zero (long)"
#line 590 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2686 "parser_gen.cpp"
                    break;

                    case 55:  // projectionCommon: "1 (double)"
#line 593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2694 "parser_gen.cpp"
                    break;

                    case 56:  // projectionCommon: "-1 (double)"
#line 596 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2702 "parser_gen.cpp"
                    break;

                    case 57:  // projectionCommon: "arbitrary double"
#line 599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2710 "parser_gen.cpp"
                    break;

                    case 58:  // projectionCommon: "zero (double)"
#line 602 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2718 "parser_gen.cpp"
                    break;

                    case 59:  // projectionCommon: "1 (decimal)"
#line 605 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2726 "parser_gen.cpp"
                    break;

                    case 60:  // projectionCommon: "-1 (decimal)"
#line 608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2734 "parser_gen.cpp"
                    break;

                    case 61:  // projectionCommon: "arbitrary decimal"
#line 611 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2742 "parser_gen.cpp"
                    break;

                    case 62:  // projectionCommon: "zero (decimal)"
#line 614 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2750 "parser_gen.cpp"
                    break;

                    case 63:  // projectionCommon: "true"
#line 617 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2758 "parser_gen.cpp"
                    break;

                    case 64:  // projectionCommon: "false"
#line 620 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2766 "parser_gen.cpp"
                    break;

                    case 65:  // projectionCommon: timestamp
#line 623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2772 "parser_gen.cpp"
                    break;

                    case 66:  // projectionCommon: minKey
#line 624 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2778 "parser_gen.cpp"
                    break;

                    case 67:  // projectionCommon: maxKey
#line 625 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2784 "parser_gen.cpp"
                    break;

                    case 68:  // projectionCommon: expressionArray
#line 626 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2790 "parser_gen.cpp"
                    break;

                    case 69:  // aggregationProjectionFieldname: projectionFieldname
#line 631 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2800 "parser_gen.cpp"
                    break;

                    case 70:  // projectionFieldname: "fieldname"
#line 640 "grammar.yy"
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
#line 2816 "parser_gen.cpp"
                    break;

                    case 71:  // projectionFieldname: argAsProjectionPath
#line 651 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2822 "parser_gen.cpp"
                    break;

                    case 72:  // projectionFieldname: "fieldname containing dotted path"
#line 652 "grammar.yy"
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
#line 2838 "parser_gen.cpp"
                    break;

                    case 73:  // aggregationProjectionObject: "object"
                              // aggregationProjectionObjectFields "end of object"
#line 667 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2846 "parser_gen.cpp"
                    break;

                    case 74:  // aggregationProjectionObjectFields: aggregationProjectionObjectField
#line 674 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2855 "parser_gen.cpp"
                    break;

                    case 75:  // aggregationProjectionObjectFields:
                              // aggregationProjectionObjectFields aggregationProjectionObjectField
#line 678 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2864 "parser_gen.cpp"
                    break;

                    case 76:  // aggregationProjectionObjectField: idAsProjectionPath
                              // aggregationProjection
#line 686 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2872 "parser_gen.cpp"
                    break;

                    case 77:  // aggregationProjectionObjectField: aggregationProjectionFieldname
                              // aggregationProjection
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2880 "parser_gen.cpp"
                    break;

                    case 78:  // matchExpression: "object" predicates "end of object"
#line 695 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2888 "parser_gen.cpp"
                    break;

                    case 79:  // predicates: %empty
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2896 "parser_gen.cpp"
                    break;

                    case 80:  // predicates: predicates predicate
#line 704 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2905 "parser_gen.cpp"
                    break;

                    case 81:  // predicate: fieldPredicate
#line 711 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2911 "parser_gen.cpp"
                    break;

                    case 82:  // predicate: commentExpr
#line 712 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2917 "parser_gen.cpp"
                    break;

                    case 83:  // predicate: logicalExpr
#line 715 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2923 "parser_gen.cpp"
                    break;

                    case 84:  // predicate: matchExpr
#line 716 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2929 "parser_gen.cpp"
                    break;

                    case 85:  // predicate: matchText
#line 717 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2935 "parser_gen.cpp"
                    break;

                    case 86:  // predicate: matchWhere
#line 718 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2941 "parser_gen.cpp"
                    break;

                    case 87:  // fieldPredicate: predFieldname predValue
#line 721 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2949 "parser_gen.cpp"
                    break;

                    case 88:  // predValue: simpleValue
#line 730 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2955 "parser_gen.cpp"
                    break;

                    case 89:  // predValue: "object" compoundMatchExprs "end of object"
#line 731 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2963 "parser_gen.cpp"
                    break;

                    case 90:  // compoundMatchExprs: %empty
#line 737 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2971 "parser_gen.cpp"
                    break;

                    case 91:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 740 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2980 "parser_gen.cpp"
                    break;

                    case 92:  // operatorExpression: notExpr
#line 748 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2986 "parser_gen.cpp"
                    break;

                    case 93:  // operatorExpression: existsExpr
#line 748 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2992 "parser_gen.cpp"
                    break;

                    case 94:  // operatorExpression: typeExpr
#line 748 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2998 "parser_gen.cpp"
                    break;

                    case 95:  // operatorExpression: matchMod
#line 748 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 3004 "parser_gen.cpp"
                    break;

                    case 96:  // existsExpr: EXISTS value
#line 752 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3012 "parser_gen.cpp"
                    break;

                    case 97:  // typeArray: "array" typeValues "end of array"
#line 758 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3020 "parser_gen.cpp"
                    break;

                    case 98:  // typeValues: %empty
#line 764 "grammar.yy"
                    {
                    }
#line 3026 "parser_gen.cpp"
                    break;

                    case 99:  // typeValues: typeValues typeValue
#line 765 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3035 "parser_gen.cpp"
                    break;

                    case 100:  // typeValue: num
#line 772 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3041 "parser_gen.cpp"
                    break;

                    case 101:  // typeValue: string
#line 772 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3047 "parser_gen.cpp"
                    break;

                    case 102:  // typeExpr: TYPE typeValue
#line 776 "grammar.yy"
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
#line 3061 "parser_gen.cpp"
                    break;

                    case 103:  // typeExpr: TYPE typeArray
#line 785 "grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 3073 "parser_gen.cpp"
                    break;

                    case 104:  // commentExpr: COMMENT value
#line 795 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3081 "parser_gen.cpp"
                    break;

                    case 105:  // notExpr: NOT regex
#line 801 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3089 "parser_gen.cpp"
                    break;

                    case 106:  // notExpr: NOT "object" compoundMatchExprs operatorExpression "end
                               // of object"
#line 806 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 3100 "parser_gen.cpp"
                    break;

                    case 107:  // matchMod: MOD "array" num num "end of array"
#line 815 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::matchMod,
                            CNode{CNode::ArrayChildren{
                                YY_MOVE(yystack_[2].value.as<CNode>()),
                                YY_MOVE(yystack_[1].value.as<CNode>()),
                            }}};
                    }
#line 3111 "parser_gen.cpp"
                    break;

                    case 108:  // logicalExpr: logicalExprField "array" additionalExprs
                               // matchExpression "end of array"
#line 825 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 3121 "parser_gen.cpp"
                    break;

                    case 109:  // logicalExprField: AND
#line 833 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 3127 "parser_gen.cpp"
                    break;

                    case 110:  // logicalExprField: OR
#line 834 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 3133 "parser_gen.cpp"
                    break;

                    case 111:  // logicalExprField: NOR
#line 835 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 3139 "parser_gen.cpp"
                    break;

                    case 112:  // additionalExprs: %empty
#line 838 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 3147 "parser_gen.cpp"
                    break;

                    case 113:  // additionalExprs: additionalExprs matchExpression
#line 841 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3156 "parser_gen.cpp"
                    break;

                    case 114:  // predFieldname: idAsUserFieldname
#line 848 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3162 "parser_gen.cpp"
                    break;

                    case 115:  // predFieldname: argAsUserFieldname
#line 848 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3168 "parser_gen.cpp"
                    break;

                    case 116:  // predFieldname: invariableUserFieldname
#line 848 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3174 "parser_gen.cpp"
                    break;

                    case 117:  // invariableUserFieldname: "fieldname"
#line 851 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3182 "parser_gen.cpp"
                    break;

                    case 118:  // matchExpr: EXPR expression
#line 857 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::expr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3190 "parser_gen.cpp"
                    break;

                    case 119:  // matchText: TEXT START_ORDERED_OBJECT textArgCaseSensitive
                               // textArgDiacriticSensitive textArgLanguage textArgSearch "end of
                               // object"
#line 869 "grammar.yy"
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
#line 3206 "parser_gen.cpp"
                    break;

                    case 120:  // textArgCaseSensitive: %empty
#line 882 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3214 "parser_gen.cpp"
                    break;

                    case 121:  // textArgCaseSensitive: "$caseSensitive argument" bool
#line 885 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3222 "parser_gen.cpp"
                    break;

                    case 122:  // textArgDiacriticSensitive: %empty
#line 890 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3230 "parser_gen.cpp"
                    break;

                    case 123:  // textArgDiacriticSensitive: "$diacriticSensitive argument" bool
#line 893 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3238 "parser_gen.cpp"
                    break;

                    case 124:  // textArgLanguage: %empty
#line 898 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::absentKey};
                    }
#line 3246 "parser_gen.cpp"
                    break;

                    case 125:  // textArgLanguage: "$language argument" string
#line 901 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3254 "parser_gen.cpp"
                    break;

                    case 126:  // textArgSearch: "$search argument" string
#line 906 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3262 "parser_gen.cpp"
                    break;

                    case 127:  // matchWhere: WHERE string
#line 912 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::where, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3268 "parser_gen.cpp"
                    break;

                    case 128:  // matchWhere: WHERE javascript
#line 913 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::where, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3274 "parser_gen.cpp"
                    break;

                    case 129:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 919 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 3282 "parser_gen.cpp"
                    break;

                    case 130:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 922 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 3290 "parser_gen.cpp"
                    break;

                    case 131:  // stageAsUserFieldname: STAGE_SKIP
#line 925 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 3298 "parser_gen.cpp"
                    break;

                    case 132:  // stageAsUserFieldname: STAGE_LIMIT
#line 928 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 3306 "parser_gen.cpp"
                    break;

                    case 133:  // stageAsUserFieldname: STAGE_MATCH
#line 931 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$match"};
                    }
#line 3314 "parser_gen.cpp"
                    break;

                    case 134:  // stageAsUserFieldname: STAGE_PROJECT
#line 934 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 3322 "parser_gen.cpp"
                    break;

                    case 135:  // stageAsUserFieldname: STAGE_SAMPLE
#line 937 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 3330 "parser_gen.cpp"
                    break;

                    case 136:  // argAsUserFieldname: arg
#line 943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3338 "parser_gen.cpp"
                    break;

                    case 137:  // argAsProjectionPath: arg
#line 949 "grammar.yy"
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
#line 3353 "parser_gen.cpp"
                    break;

                    case 138:  // arg: "coll argument"
#line 965 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 3361 "parser_gen.cpp"
                    break;

                    case 139:  // arg: "pipeline argument"
#line 968 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 3369 "parser_gen.cpp"
                    break;

                    case 140:  // arg: "size argument"
#line 971 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 3377 "parser_gen.cpp"
                    break;

                    case 141:  // arg: "input argument"
#line 974 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 3385 "parser_gen.cpp"
                    break;

                    case 142:  // arg: "to argument"
#line 977 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 3393 "parser_gen.cpp"
                    break;

                    case 143:  // arg: "onError argument"
#line 980 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 3401 "parser_gen.cpp"
                    break;

                    case 144:  // arg: "onNull argument"
#line 983 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 3409 "parser_gen.cpp"
                    break;

                    case 145:  // arg: "dateString argument"
#line 986 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 3417 "parser_gen.cpp"
                    break;

                    case 146:  // arg: "format argument"
#line 989 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 3425 "parser_gen.cpp"
                    break;

                    case 147:  // arg: "timezone argument"
#line 992 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 3433 "parser_gen.cpp"
                    break;

                    case 148:  // arg: "date argument"
#line 995 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 3441 "parser_gen.cpp"
                    break;

                    case 149:  // arg: "chars argument"
#line 998 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 3449 "parser_gen.cpp"
                    break;

                    case 150:  // arg: "regex argument"
#line 1001 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3457 "parser_gen.cpp"
                    break;

                    case 151:  // arg: "options argument"
#line 1004 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3465 "parser_gen.cpp"
                    break;

                    case 152:  // arg: "find argument"
#line 1007 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3473 "parser_gen.cpp"
                    break;

                    case 153:  // arg: "replacement argument"
#line 1010 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3481 "parser_gen.cpp"
                    break;

                    case 154:  // arg: "hour argument"
#line 1013 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"hour"};
                    }
#line 3489 "parser_gen.cpp"
                    break;

                    case 155:  // arg: "year argument"
#line 1016 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"year"};
                    }
#line 3497 "parser_gen.cpp"
                    break;

                    case 156:  // arg: "minute argument"
#line 1019 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"minute"};
                    }
#line 3505 "parser_gen.cpp"
                    break;

                    case 157:  // arg: "second argument"
#line 1022 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"second"};
                    }
#line 3513 "parser_gen.cpp"
                    break;

                    case 158:  // arg: "millisecond argument"
#line 1025 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"millisecond"};
                    }
#line 3521 "parser_gen.cpp"
                    break;

                    case 159:  // arg: "day argument"
#line 1028 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"day"};
                    }
#line 3529 "parser_gen.cpp"
                    break;

                    case 160:  // arg: "ISO day of week argument"
#line 1031 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoDayOfWeek"};
                    }
#line 3537 "parser_gen.cpp"
                    break;

                    case 161:  // arg: "ISO week argument"
#line 1034 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeek"};
                    }
#line 3545 "parser_gen.cpp"
                    break;

                    case 162:  // arg: "ISO week year argument"
#line 1037 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeekYear"};
                    }
#line 3553 "parser_gen.cpp"
                    break;

                    case 163:  // arg: "ISO 8601 argument"
#line 1040 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"iso8601"};
                    }
#line 3561 "parser_gen.cpp"
                    break;

                    case 164:  // arg: "month argument"
#line 1043 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"month"};
                    }
#line 3569 "parser_gen.cpp"
                    break;

                    case 165:  // arg: "$search argument"
#line 1046 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$search"};
                    }
#line 3577 "parser_gen.cpp"
                    break;

                    case 166:  // arg: "$language argument"
#line 1049 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$language"};
                    }
#line 3585 "parser_gen.cpp"
                    break;

                    case 167:  // arg: "$caseSensitive argument"
#line 1052 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$caseSensitive"};
                    }
#line 3593 "parser_gen.cpp"
                    break;

                    case 168:  // arg: "$diacriticSensitive argument"
#line 1055 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"$diacriticSensitive"};
                    }
#line 3601 "parser_gen.cpp"
                    break;

                    case 169:  // arg: "as argument"
#line 1058 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"as"};
                    }
#line 3609 "parser_gen.cpp"
                    break;

                    case 170:  // arg: "cond argument"
#line 1061 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"cond"};
                    }
#line 3617 "parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: ADD
#line 1069 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3625 "parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: ATAN2
#line 1072 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3633 "parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: AND
#line 1075 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3641 "parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: CONST_EXPR
#line 1078 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3649 "parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: LITERAL
#line 1081 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3657 "parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: OR
#line 1084 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3665 "parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: NOT
#line 1087 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3673 "parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: CMP
#line 1090 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3681 "parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: EQ
#line 1093 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3689 "parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: GT
#line 1096 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3697 "parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: GTE
#line 1099 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3705 "parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: LT
#line 1102 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3713 "parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: LTE
#line 1105 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3721 "parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: NE
#line 1108 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3729 "parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: CONVERT
#line 1111 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3737 "parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: TO_BOOL
#line 1114 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3745 "parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: TO_DATE
#line 1117 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3753 "parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: TO_DECIMAL
#line 1120 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3761 "parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: TO_DOUBLE
#line 1123 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3769 "parser_gen.cpp"
                    break;

                    case 190:  // aggExprAsUserFieldname: TO_INT
#line 1126 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3777 "parser_gen.cpp"
                    break;

                    case 191:  // aggExprAsUserFieldname: TO_LONG
#line 1129 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3785 "parser_gen.cpp"
                    break;

                    case 192:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 1132 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3793 "parser_gen.cpp"
                    break;

                    case 193:  // aggExprAsUserFieldname: TO_STRING
#line 1135 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3801 "parser_gen.cpp"
                    break;

                    case 194:  // aggExprAsUserFieldname: TYPE
#line 1138 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3809 "parser_gen.cpp"
                    break;

                    case 195:  // aggExprAsUserFieldname: ABS
#line 1141 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3817 "parser_gen.cpp"
                    break;

                    case 196:  // aggExprAsUserFieldname: CEIL
#line 1144 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3825 "parser_gen.cpp"
                    break;

                    case 197:  // aggExprAsUserFieldname: DIVIDE
#line 1147 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3833 "parser_gen.cpp"
                    break;

                    case 198:  // aggExprAsUserFieldname: EXPONENT
#line 1150 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3841 "parser_gen.cpp"
                    break;

                    case 199:  // aggExprAsUserFieldname: FLOOR
#line 1153 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3849 "parser_gen.cpp"
                    break;

                    case 200:  // aggExprAsUserFieldname: LN
#line 1156 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3857 "parser_gen.cpp"
                    break;

                    case 201:  // aggExprAsUserFieldname: LOG
#line 1159 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3865 "parser_gen.cpp"
                    break;

                    case 202:  // aggExprAsUserFieldname: LOGTEN
#line 1162 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3873 "parser_gen.cpp"
                    break;

                    case 203:  // aggExprAsUserFieldname: MOD
#line 1165 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3881 "parser_gen.cpp"
                    break;

                    case 204:  // aggExprAsUserFieldname: MULTIPLY
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3889 "parser_gen.cpp"
                    break;

                    case 205:  // aggExprAsUserFieldname: POW
#line 1171 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3897 "parser_gen.cpp"
                    break;

                    case 206:  // aggExprAsUserFieldname: ROUND
#line 1174 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3905 "parser_gen.cpp"
                    break;

                    case 207:  // aggExprAsUserFieldname: "slice"
#line 1177 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3913 "parser_gen.cpp"
                    break;

                    case 208:  // aggExprAsUserFieldname: SQRT
#line 1180 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3921 "parser_gen.cpp"
                    break;

                    case 209:  // aggExprAsUserFieldname: SUBTRACT
#line 1183 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3929 "parser_gen.cpp"
                    break;

                    case 210:  // aggExprAsUserFieldname: TRUNC
#line 1186 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3937 "parser_gen.cpp"
                    break;

                    case 211:  // aggExprAsUserFieldname: CONCAT
#line 1189 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3945 "parser_gen.cpp"
                    break;

                    case 212:  // aggExprAsUserFieldname: DATE_FROM_PARTS
#line 1192 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromParts"};
                    }
#line 3953 "parser_gen.cpp"
                    break;

                    case 213:  // aggExprAsUserFieldname: DATE_TO_PARTS
#line 1195 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToParts"};
                    }
#line 3961 "parser_gen.cpp"
                    break;

                    case 214:  // aggExprAsUserFieldname: DAY_OF_MONTH
#line 1198 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfMonth"};
                    }
#line 3969 "parser_gen.cpp"
                    break;

                    case 215:  // aggExprAsUserFieldname: DAY_OF_WEEK
#line 1201 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfWeek"};
                    }
#line 3977 "parser_gen.cpp"
                    break;

                    case 216:  // aggExprAsUserFieldname: DAY_OF_YEAR
#line 1204 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfYear"};
                    }
#line 3985 "parser_gen.cpp"
                    break;

                    case 217:  // aggExprAsUserFieldname: HOUR
#line 1207 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$hour"};
                    }
#line 3993 "parser_gen.cpp"
                    break;

                    case 218:  // aggExprAsUserFieldname: ISO_DAY_OF_WEEK
#line 1210 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoDayOfWeek"};
                    }
#line 4001 "parser_gen.cpp"
                    break;

                    case 219:  // aggExprAsUserFieldname: ISO_WEEK
#line 1213 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeek"};
                    }
#line 4009 "parser_gen.cpp"
                    break;

                    case 220:  // aggExprAsUserFieldname: ISO_WEEK_YEAR
#line 1216 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeekYear"};
                    }
#line 4017 "parser_gen.cpp"
                    break;

                    case 221:  // aggExprAsUserFieldname: MILLISECOND
#line 1219 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$millisecond"};
                    }
#line 4025 "parser_gen.cpp"
                    break;

                    case 222:  // aggExprAsUserFieldname: MINUTE
#line 1222 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$minute"};
                    }
#line 4033 "parser_gen.cpp"
                    break;

                    case 223:  // aggExprAsUserFieldname: MONTH
#line 1225 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$month"};
                    }
#line 4041 "parser_gen.cpp"
                    break;

                    case 224:  // aggExprAsUserFieldname: SECOND
#line 1228 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$second"};
                    }
#line 4049 "parser_gen.cpp"
                    break;

                    case 225:  // aggExprAsUserFieldname: WEEK
#line 1231 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$week"};
                    }
#line 4057 "parser_gen.cpp"
                    break;

                    case 226:  // aggExprAsUserFieldname: YEAR
#line 1234 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$year"};
                    }
#line 4065 "parser_gen.cpp"
                    break;

                    case 227:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 1237 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 4073 "parser_gen.cpp"
                    break;

                    case 228:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 1240 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 4081 "parser_gen.cpp"
                    break;

                    case 229:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 1243 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 4089 "parser_gen.cpp"
                    break;

                    case 230:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 1246 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 4097 "parser_gen.cpp"
                    break;

                    case 231:  // aggExprAsUserFieldname: LTRIM
#line 1249 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 4105 "parser_gen.cpp"
                    break;

                    case 232:  // aggExprAsUserFieldname: META
#line 1252 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 4113 "parser_gen.cpp"
                    break;

                    case 233:  // aggExprAsUserFieldname: REGEX_FIND
#line 1255 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 4121 "parser_gen.cpp"
                    break;

                    case 234:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 1258 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 4129 "parser_gen.cpp"
                    break;

                    case 235:  // aggExprAsUserFieldname: REGEX_MATCH
#line 1261 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 4137 "parser_gen.cpp"
                    break;

                    case 236:  // aggExprAsUserFieldname: REPLACE_ONE
#line 1264 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 4145 "parser_gen.cpp"
                    break;

                    case 237:  // aggExprAsUserFieldname: REPLACE_ALL
#line 1267 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 4153 "parser_gen.cpp"
                    break;

                    case 238:  // aggExprAsUserFieldname: RTRIM
#line 1270 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 4161 "parser_gen.cpp"
                    break;

                    case 239:  // aggExprAsUserFieldname: SPLIT
#line 1273 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 4169 "parser_gen.cpp"
                    break;

                    case 240:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 1276 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 4177 "parser_gen.cpp"
                    break;

                    case 241:  // aggExprAsUserFieldname: STR_LEN_CP
#line 1279 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 4185 "parser_gen.cpp"
                    break;

                    case 242:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 4193 "parser_gen.cpp"
                    break;

                    case 243:  // aggExprAsUserFieldname: SUBSTR
#line 1285 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 4201 "parser_gen.cpp"
                    break;

                    case 244:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 1288 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 4209 "parser_gen.cpp"
                    break;

                    case 245:  // aggExprAsUserFieldname: SUBSTR_CP
#line 1291 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 4217 "parser_gen.cpp"
                    break;

                    case 246:  // aggExprAsUserFieldname: TO_LOWER
#line 1294 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 4225 "parser_gen.cpp"
                    break;

                    case 247:  // aggExprAsUserFieldname: TRIM
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 4233 "parser_gen.cpp"
                    break;

                    case 248:  // aggExprAsUserFieldname: TO_UPPER
#line 1300 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 4241 "parser_gen.cpp"
                    break;

                    case 249:  // aggExprAsUserFieldname: "allElementsTrue"
#line 1303 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 4249 "parser_gen.cpp"
                    break;

                    case 250:  // aggExprAsUserFieldname: "anyElementTrue"
#line 1306 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 4257 "parser_gen.cpp"
                    break;

                    case 251:  // aggExprAsUserFieldname: "setDifference"
#line 1309 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 4265 "parser_gen.cpp"
                    break;

                    case 252:  // aggExprAsUserFieldname: "setEquals"
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 4273 "parser_gen.cpp"
                    break;

                    case 253:  // aggExprAsUserFieldname: "setIntersection"
#line 1315 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 4281 "parser_gen.cpp"
                    break;

                    case 254:  // aggExprAsUserFieldname: "setIsSubset"
#line 1318 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 4289 "parser_gen.cpp"
                    break;

                    case 255:  // aggExprAsUserFieldname: "setUnion"
#line 1321 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 4297 "parser_gen.cpp"
                    break;

                    case 256:  // aggExprAsUserFieldname: SIN
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 4305 "parser_gen.cpp"
                    break;

                    case 257:  // aggExprAsUserFieldname: COS
#line 1327 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 4313 "parser_gen.cpp"
                    break;

                    case 258:  // aggExprAsUserFieldname: TAN
#line 1330 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 4321 "parser_gen.cpp"
                    break;

                    case 259:  // aggExprAsUserFieldname: SINH
#line 1333 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 4329 "parser_gen.cpp"
                    break;

                    case 260:  // aggExprAsUserFieldname: COSH
#line 1336 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 4337 "parser_gen.cpp"
                    break;

                    case 261:  // aggExprAsUserFieldname: TANH
#line 1339 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 4345 "parser_gen.cpp"
                    break;

                    case 262:  // aggExprAsUserFieldname: ASIN
#line 1342 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 4353 "parser_gen.cpp"
                    break;

                    case 263:  // aggExprAsUserFieldname: ACOS
#line 1345 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 4361 "parser_gen.cpp"
                    break;

                    case 264:  // aggExprAsUserFieldname: ATAN
#line 1348 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 4369 "parser_gen.cpp"
                    break;

                    case 265:  // aggExprAsUserFieldname: ASINH
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 4377 "parser_gen.cpp"
                    break;

                    case 266:  // aggExprAsUserFieldname: ACOSH
#line 1354 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 4385 "parser_gen.cpp"
                    break;

                    case 267:  // aggExprAsUserFieldname: ATANH
#line 1357 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 4393 "parser_gen.cpp"
                    break;

                    case 268:  // aggExprAsUserFieldname: DEGREES_TO_RADIANS
#line 1360 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 4401 "parser_gen.cpp"
                    break;

                    case 269:  // aggExprAsUserFieldname: RADIANS_TO_DEGREES
#line 1363 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 4409 "parser_gen.cpp"
                    break;

                    case 270:  // aggExprAsUserFieldname: ARRAY_ELEM_AT
#line 1366 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$arrayElemAt"};
                    }
#line 4417 "parser_gen.cpp"
                    break;

                    case 271:  // aggExprAsUserFieldname: ARRAY_TO_OBJECT
#line 1369 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$arrayToObject"};
                    }
#line 4425 "parser_gen.cpp"
                    break;

                    case 272:  // aggExprAsUserFieldname: CONCAT_ARRAYS
#line 1372 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concatArrays"};
                    }
#line 4433 "parser_gen.cpp"
                    break;

                    case 273:  // aggExprAsUserFieldname: FILTER
#line 1375 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$filter"};
                    }
#line 4441 "parser_gen.cpp"
                    break;

                    case 274:  // aggExprAsUserFieldname: FIRST
#line 1378 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$first"};
                    }
#line 4449 "parser_gen.cpp"
                    break;

                    case 275:  // aggExprAsUserFieldname: IN
#line 1381 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$in"};
                    }
#line 4457 "parser_gen.cpp"
                    break;

                    case 276:  // aggExprAsUserFieldname: INDEX_OF_ARRAY
#line 1384 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfArray"};
                    }
#line 4465 "parser_gen.cpp"
                    break;

                    case 277:  // aggExprAsUserFieldname: IS_ARRAY
#line 1387 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isArray"};
                    }
#line 4473 "parser_gen.cpp"
                    break;

                    case 278:  // string: "string"
#line 1394 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 4481 "parser_gen.cpp"
                    break;

                    case 279:  // string: "geoNearDistance"
#line 1399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 4489 "parser_gen.cpp"
                    break;

                    case 280:  // string: "geoNearPoint"
#line 1402 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 4497 "parser_gen.cpp"
                    break;

                    case 281:  // string: "indexKey"
#line 1405 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 4505 "parser_gen.cpp"
                    break;

                    case 282:  // string: "randVal"
#line 1408 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 4513 "parser_gen.cpp"
                    break;

                    case 283:  // string: "recordId"
#line 1411 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 4521 "parser_gen.cpp"
                    break;

                    case 284:  // string: "searchHighlights"
#line 1414 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 4529 "parser_gen.cpp"
                    break;

                    case 285:  // string: "searchScore"
#line 1417 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 4537 "parser_gen.cpp"
                    break;

                    case 286:  // string: "sortKey"
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 4545 "parser_gen.cpp"
                    break;

                    case 287:  // string: "textScore"
#line 1423 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 4553 "parser_gen.cpp"
                    break;

                    case 288:  // aggregationFieldPath: "$-prefixed string"
#line 1429 "grammar.yy"
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
#line 4569 "parser_gen.cpp"
                    break;

                    case 289:  // variable: "$$-prefixed string"
#line 1443 "grammar.yy"
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
#line 4585 "parser_gen.cpp"
                    break;

                    case 290:  // binary: "BinData"
#line 1457 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 4593 "parser_gen.cpp"
                    break;

                    case 291:  // undefined: "undefined"
#line 1463 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 4601 "parser_gen.cpp"
                    break;

                    case 292:  // objectId: "ObjectID"
#line 1469 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 4609 "parser_gen.cpp"
                    break;

                    case 293:  // date: "Date"
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 4617 "parser_gen.cpp"
                    break;

                    case 294:  // null: "null"
#line 1481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 4625 "parser_gen.cpp"
                    break;

                    case 295:  // regex: "regex"
#line 1487 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 4633 "parser_gen.cpp"
                    break;

                    case 296:  // dbPointer: "dbPointer"
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 4641 "parser_gen.cpp"
                    break;

                    case 297:  // javascript: "Code"
#line 1499 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 4649 "parser_gen.cpp"
                    break;

                    case 298:  // symbol: "Symbol"
#line 1505 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 4657 "parser_gen.cpp"
                    break;

                    case 299:  // javascriptWScope: "CodeWScope"
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 4665 "parser_gen.cpp"
                    break;

                    case 300:  // timestamp: "Timestamp"
#line 1517 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 4673 "parser_gen.cpp"
                    break;

                    case 301:  // minKey: "minKey"
#line 1523 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 4681 "parser_gen.cpp"
                    break;

                    case 302:  // maxKey: "maxKey"
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 4689 "parser_gen.cpp"
                    break;

                    case 303:  // int: "arbitrary integer"
#line 1535 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 4697 "parser_gen.cpp"
                    break;

                    case 304:  // int: "zero (int)"
#line 1538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 4705 "parser_gen.cpp"
                    break;

                    case 305:  // int: "1 (int)"
#line 1541 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 4713 "parser_gen.cpp"
                    break;

                    case 306:  // int: "-1 (int)"
#line 1544 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 4721 "parser_gen.cpp"
                    break;

                    case 307:  // long: "arbitrary long"
#line 1550 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 4729 "parser_gen.cpp"
                    break;

                    case 308:  // long: "zero (long)"
#line 1553 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 4737 "parser_gen.cpp"
                    break;

                    case 309:  // long: "1 (long)"
#line 1556 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 4745 "parser_gen.cpp"
                    break;

                    case 310:  // long: "-1 (long)"
#line 1559 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 4753 "parser_gen.cpp"
                    break;

                    case 311:  // double: "arbitrary double"
#line 1565 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 4761 "parser_gen.cpp"
                    break;

                    case 312:  // double: "zero (double)"
#line 1568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 4769 "parser_gen.cpp"
                    break;

                    case 313:  // double: "1 (double)"
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4777 "parser_gen.cpp"
                    break;

                    case 314:  // double: "-1 (double)"
#line 1574 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4785 "parser_gen.cpp"
                    break;

                    case 315:  // decimal: "arbitrary decimal"
#line 1580 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4793 "parser_gen.cpp"
                    break;

                    case 316:  // decimal: "zero (decimal)"
#line 1583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4801 "parser_gen.cpp"
                    break;

                    case 317:  // decimal: "1 (decimal)"
#line 1586 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4809 "parser_gen.cpp"
                    break;

                    case 318:  // decimal: "-1 (decimal)"
#line 1589 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4817 "parser_gen.cpp"
                    break;

                    case 319:  // bool: "true"
#line 1595 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4825 "parser_gen.cpp"
                    break;

                    case 320:  // bool: "false"
#line 1598 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4833 "parser_gen.cpp"
                    break;

                    case 321:  // simpleValue: string
#line 1604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4839 "parser_gen.cpp"
                    break;

                    case 322:  // simpleValue: aggregationFieldPath
#line 1605 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4845 "parser_gen.cpp"
                    break;

                    case 323:  // simpleValue: variable
#line 1606 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4851 "parser_gen.cpp"
                    break;

                    case 324:  // simpleValue: binary
#line 1607 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4857 "parser_gen.cpp"
                    break;

                    case 325:  // simpleValue: undefined
#line 1608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4863 "parser_gen.cpp"
                    break;

                    case 326:  // simpleValue: objectId
#line 1609 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4869 "parser_gen.cpp"
                    break;

                    case 327:  // simpleValue: date
#line 1610 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4875 "parser_gen.cpp"
                    break;

                    case 328:  // simpleValue: null
#line 1611 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4881 "parser_gen.cpp"
                    break;

                    case 329:  // simpleValue: regex
#line 1612 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4887 "parser_gen.cpp"
                    break;

                    case 330:  // simpleValue: dbPointer
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4893 "parser_gen.cpp"
                    break;

                    case 331:  // simpleValue: javascript
#line 1614 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4899 "parser_gen.cpp"
                    break;

                    case 332:  // simpleValue: symbol
#line 1615 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4905 "parser_gen.cpp"
                    break;

                    case 333:  // simpleValue: javascriptWScope
#line 1616 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4911 "parser_gen.cpp"
                    break;

                    case 334:  // simpleValue: int
#line 1617 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4917 "parser_gen.cpp"
                    break;

                    case 335:  // simpleValue: long
#line 1618 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4923 "parser_gen.cpp"
                    break;

                    case 336:  // simpleValue: double
#line 1619 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4929 "parser_gen.cpp"
                    break;

                    case 337:  // simpleValue: decimal
#line 1620 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4935 "parser_gen.cpp"
                    break;

                    case 338:  // simpleValue: bool
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4941 "parser_gen.cpp"
                    break;

                    case 339:  // simpleValue: timestamp
#line 1622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4947 "parser_gen.cpp"
                    break;

                    case 340:  // simpleValue: minKey
#line 1623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4953 "parser_gen.cpp"
                    break;

                    case 341:  // simpleValue: maxKey
#line 1624 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4959 "parser_gen.cpp"
                    break;

                    case 342:  // expressions: %empty
#line 1631 "grammar.yy"
                    {
                    }
#line 4965 "parser_gen.cpp"
                    break;

                    case 343:  // expressions: expressions expression
#line 1632 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4974 "parser_gen.cpp"
                    break;

                    case 344:  // expression: simpleValue
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4980 "parser_gen.cpp"
                    break;

                    case 345:  // expression: expressionObject
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4986 "parser_gen.cpp"
                    break;

                    case 346:  // expression: expressionArray
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4992 "parser_gen.cpp"
                    break;

                    case 347:  // expression: aggregationOperator
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4998 "parser_gen.cpp"
                    break;

                    case 348:  // nonArrayExpression: simpleValue
#line 1643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5004 "parser_gen.cpp"
                    break;

                    case 349:  // nonArrayExpression: nonArrayCompoundExpression
#line 1643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5010 "parser_gen.cpp"
                    break;

                    case 350:  // nonArrayNonObjExpression: simpleValue
#line 1647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5016 "parser_gen.cpp"
                    break;

                    case 351:  // nonArrayNonObjExpression: aggregationOperator
#line 1647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5022 "parser_gen.cpp"
                    break;

                    case 352:  // nonArrayCompoundExpression: expressionObject
#line 1651 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5028 "parser_gen.cpp"
                    break;

                    case 353:  // nonArrayCompoundExpression: aggregationOperator
#line 1651 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5034 "parser_gen.cpp"
                    break;

                    case 354:  // aggregationOperator: aggregationOperatorWithoutSlice
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5040 "parser_gen.cpp"
                    break;

                    case 355:  // aggregationOperator: slice
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5046 "parser_gen.cpp"
                    break;

                    case 356:  // aggregationOperatorWithoutSlice: maths
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5052 "parser_gen.cpp"
                    break;

                    case 357:  // aggregationOperatorWithoutSlice: boolExprs
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5058 "parser_gen.cpp"
                    break;

                    case 358:  // aggregationOperatorWithoutSlice: literalEscapes
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5064 "parser_gen.cpp"
                    break;

                    case 359:  // aggregationOperatorWithoutSlice: compExprs
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5070 "parser_gen.cpp"
                    break;

                    case 360:  // aggregationOperatorWithoutSlice: typeExpression
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5076 "parser_gen.cpp"
                    break;

                    case 361:  // aggregationOperatorWithoutSlice: stringExps
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5082 "parser_gen.cpp"
                    break;

                    case 362:  // aggregationOperatorWithoutSlice: setExpression
#line 1659 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5088 "parser_gen.cpp"
                    break;

                    case 363:  // aggregationOperatorWithoutSlice: trig
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5094 "parser_gen.cpp"
                    break;

                    case 364:  // aggregationOperatorWithoutSlice: meta
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5100 "parser_gen.cpp"
                    break;

                    case 365:  // aggregationOperatorWithoutSlice: dateExps
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5106 "parser_gen.cpp"
                    break;

                    case 366:  // aggregationOperatorWithoutSlice: arrayExps
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5112 "parser_gen.cpp"
                    break;

                    case 367:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1665 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5120 "parser_gen.cpp"
                    break;

                    case 368:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1672 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5128 "parser_gen.cpp"
                    break;

                    case 369:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1678 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5137 "parser_gen.cpp"
                    break;

                    case 370:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1682 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5146 "parser_gen.cpp"
                    break;

                    case 371:  // expressionArray: "array" expressions "end of array"
#line 1691 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5154 "parser_gen.cpp"
                    break;

                    case 372:  // expressionSingletonArray: "array" expression "end of array"
#line 1698 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 5162 "parser_gen.cpp"
                    break;

                    case 373:  // singleArgExpression: nonArrayExpression
#line 1703 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5168 "parser_gen.cpp"
                    break;

                    case 374:  // singleArgExpression: expressionSingletonArray
#line 1703 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5174 "parser_gen.cpp"
                    break;

                    case 375:  // expressionObject: "object" expressionFields "end of object"
#line 1708 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5182 "parser_gen.cpp"
                    break;

                    case 376:  // expressionFields: %empty
#line 1714 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5190 "parser_gen.cpp"
                    break;

                    case 377:  // expressionFields: expressionFields expressionField
#line 1717 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5199 "parser_gen.cpp"
                    break;

                    case 378:  // expressionField: expressionFieldname expression
#line 1724 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5207 "parser_gen.cpp"
                    break;

                    case 379:  // expressionFieldname: invariableUserFieldname
#line 1731 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5213 "parser_gen.cpp"
                    break;

                    case 380:  // expressionFieldname: argAsUserFieldname
#line 1731 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5219 "parser_gen.cpp"
                    break;

                    case 381:  // expressionFieldname: idAsUserFieldname
#line 1731 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5225 "parser_gen.cpp"
                    break;

                    case 382:  // idAsUserFieldname: ID
#line 1735 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 5233 "parser_gen.cpp"
                    break;

                    case 383:  // elemMatchAsUserFieldname: "elemMatch operator"
#line 1741 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$elemMatch"};
                    }
#line 5241 "parser_gen.cpp"
                    break;

                    case 384:  // idAsProjectionPath: ID
#line 1747 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 5249 "parser_gen.cpp"
                    break;

                    case 385:  // maths: add
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5255 "parser_gen.cpp"
                    break;

                    case 386:  // maths: abs
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5261 "parser_gen.cpp"
                    break;

                    case 387:  // maths: ceil
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5267 "parser_gen.cpp"
                    break;

                    case 388:  // maths: divide
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5273 "parser_gen.cpp"
                    break;

                    case 389:  // maths: exponent
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5279 "parser_gen.cpp"
                    break;

                    case 390:  // maths: floor
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5285 "parser_gen.cpp"
                    break;

                    case 391:  // maths: ln
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5291 "parser_gen.cpp"
                    break;

                    case 392:  // maths: log
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5297 "parser_gen.cpp"
                    break;

                    case 393:  // maths: logten
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5303 "parser_gen.cpp"
                    break;

                    case 394:  // maths: mod
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5309 "parser_gen.cpp"
                    break;

                    case 395:  // maths: multiply
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5315 "parser_gen.cpp"
                    break;

                    case 396:  // maths: pow
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5321 "parser_gen.cpp"
                    break;

                    case 397:  // maths: round
#line 1753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5327 "parser_gen.cpp"
                    break;

                    case 398:  // maths: sqrt
#line 1754 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5333 "parser_gen.cpp"
                    break;

                    case 399:  // maths: subtract
#line 1754 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5339 "parser_gen.cpp"
                    break;

                    case 400:  // maths: trunc
#line 1754 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5345 "parser_gen.cpp"
                    break;

                    case 401:  // meta: "object" META "geoNearDistance" "end of object"
#line 1758 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 5353 "parser_gen.cpp"
                    break;

                    case 402:  // meta: "object" META "geoNearPoint" "end of object"
#line 1761 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 5361 "parser_gen.cpp"
                    break;

                    case 403:  // meta: "object" META "indexKey" "end of object"
#line 1764 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 5369 "parser_gen.cpp"
                    break;

                    case 404:  // meta: "object" META "randVal" "end of object"
#line 1767 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 5377 "parser_gen.cpp"
                    break;

                    case 405:  // meta: "object" META "recordId" "end of object"
#line 1770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 5385 "parser_gen.cpp"
                    break;

                    case 406:  // meta: "object" META "searchHighlights" "end of object"
#line 1773 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 5393 "parser_gen.cpp"
                    break;

                    case 407:  // meta: "object" META "searchScore" "end of object"
#line 1776 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 5401 "parser_gen.cpp"
                    break;

                    case 408:  // meta: "object" META "sortKey" "end of object"
#line 1779 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 5409 "parser_gen.cpp"
                    break;

                    case 409:  // meta: "object" META "textScore" "end of object"
#line 1782 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 5417 "parser_gen.cpp"
                    break;

                    case 410:  // trig: sin
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5423 "parser_gen.cpp"
                    break;

                    case 411:  // trig: cos
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5429 "parser_gen.cpp"
                    break;

                    case 412:  // trig: tan
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5435 "parser_gen.cpp"
                    break;

                    case 413:  // trig: sinh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5441 "parser_gen.cpp"
                    break;

                    case 414:  // trig: cosh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5447 "parser_gen.cpp"
                    break;

                    case 415:  // trig: tanh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5453 "parser_gen.cpp"
                    break;

                    case 416:  // trig: asin
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5459 "parser_gen.cpp"
                    break;

                    case 417:  // trig: acos
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5465 "parser_gen.cpp"
                    break;

                    case 418:  // trig: atan
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5471 "parser_gen.cpp"
                    break;

                    case 419:  // trig: atan2
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5477 "parser_gen.cpp"
                    break;

                    case 420:  // trig: asinh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5483 "parser_gen.cpp"
                    break;

                    case 421:  // trig: acosh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5489 "parser_gen.cpp"
                    break;

                    case 422:  // trig: atanh
#line 1787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5495 "parser_gen.cpp"
                    break;

                    case 423:  // trig: degreesToRadians
#line 1788 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5501 "parser_gen.cpp"
                    break;

                    case 424:  // trig: radiansToDegrees
#line 1788 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5507 "parser_gen.cpp"
                    break;

                    case 425:  // add: "object" ADD expressionArray "end of object"
#line 1792 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5516 "parser_gen.cpp"
                    break;

                    case 426:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1799 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5525 "parser_gen.cpp"
                    break;

                    case 427:  // abs: "object" ABS singleArgExpression "end of object"
#line 1805 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5533 "parser_gen.cpp"
                    break;

                    case 428:  // ceil: "object" CEIL singleArgExpression "end of object"
#line 1810 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5541 "parser_gen.cpp"
                    break;

                    case 429:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1815 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5550 "parser_gen.cpp"
                    break;

                    case 430:  // exponent: "object" EXPONENT singleArgExpression "end of object"
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5558 "parser_gen.cpp"
                    break;

                    case 431:  // floor: "object" FLOOR singleArgExpression "end of object"
#line 1826 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5566 "parser_gen.cpp"
                    break;

                    case 432:  // ln: "object" LN singleArgExpression "end of object"
#line 1831 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5574 "parser_gen.cpp"
                    break;

                    case 433:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1836 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5583 "parser_gen.cpp"
                    break;

                    case 434:  // logten: "object" LOGTEN singleArgExpression "end of object"
#line 1842 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5591 "parser_gen.cpp"
                    break;

                    case 435:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1847 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5600 "parser_gen.cpp"
                    break;

                    case 436:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1853 "grammar.yy"
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
#line 5612 "parser_gen.cpp"
                    break;

                    case 437:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1862 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5621 "parser_gen.cpp"
                    break;

                    case 438:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1868 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5630 "parser_gen.cpp"
                    break;

                    case 439:  // sqrt: "object" SQRT singleArgExpression "end of object"
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5638 "parser_gen.cpp"
                    break;

                    case 440:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1879 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5647 "parser_gen.cpp"
                    break;

                    case 441:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1885 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5656 "parser_gen.cpp"
                    break;

                    case 442:  // sin: "object" SIN singleArgExpression "end of object"
#line 1891 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5664 "parser_gen.cpp"
                    break;

                    case 443:  // cos: "object" COS singleArgExpression "end of object"
#line 1896 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5672 "parser_gen.cpp"
                    break;

                    case 444:  // tan: "object" TAN singleArgExpression "end of object"
#line 1901 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5680 "parser_gen.cpp"
                    break;

                    case 445:  // sinh: "object" SINH singleArgExpression "end of object"
#line 1906 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5688 "parser_gen.cpp"
                    break;

                    case 446:  // cosh: "object" COSH singleArgExpression "end of object"
#line 1911 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5696 "parser_gen.cpp"
                    break;

                    case 447:  // tanh: "object" TANH singleArgExpression "end of object"
#line 1916 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5704 "parser_gen.cpp"
                    break;

                    case 448:  // asin: "object" ASIN singleArgExpression "end of object"
#line 1921 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5712 "parser_gen.cpp"
                    break;

                    case 449:  // acos: "object" ACOS singleArgExpression "end of object"
#line 1926 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5720 "parser_gen.cpp"
                    break;

                    case 450:  // atan: "object" ATAN singleArgExpression "end of object"
#line 1931 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5728 "parser_gen.cpp"
                    break;

                    case 451:  // asinh: "object" ASINH singleArgExpression "end of object"
#line 1936 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5736 "parser_gen.cpp"
                    break;

                    case 452:  // acosh: "object" ACOSH singleArgExpression "end of object"
#line 1941 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5744 "parser_gen.cpp"
                    break;

                    case 453:  // atanh: "object" ATANH singleArgExpression "end of object"
#line 1946 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5752 "parser_gen.cpp"
                    break;

                    case 454:  // degreesToRadians: "object" DEGREES_TO_RADIANS singleArgExpression
                               // "end of object"
#line 1951 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5760 "parser_gen.cpp"
                    break;

                    case 455:  // radiansToDegrees: "object" RADIANS_TO_DEGREES singleArgExpression
                               // "end of object"
#line 1956 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5768 "parser_gen.cpp"
                    break;

                    case 456:  // boolExprs: and
#line 1962 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5774 "parser_gen.cpp"
                    break;

                    case 457:  // boolExprs: or
#line 1962 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5780 "parser_gen.cpp"
                    break;

                    case 458:  // boolExprs: not
#line 1962 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5786 "parser_gen.cpp"
                    break;

                    case 459:  // and: "object" AND expressionArray "end of object"
#line 1966 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5795 "parser_gen.cpp"
                    break;

                    case 460:  // or: "object" OR expressionArray "end of object"
#line 1973 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5804 "parser_gen.cpp"
                    break;

                    case 461:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1980 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5813 "parser_gen.cpp"
                    break;

                    case 462:  // arrayExps: arrayElemAt
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5819 "parser_gen.cpp"
                    break;

                    case 463:  // arrayExps: arrayToObject
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5825 "parser_gen.cpp"
                    break;

                    case 464:  // arrayExps: concatArrays
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5831 "parser_gen.cpp"
                    break;

                    case 465:  // arrayExps: filter
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5837 "parser_gen.cpp"
                    break;

                    case 466:  // arrayExps: first
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5843 "parser_gen.cpp"
                    break;

                    case 467:  // arrayExps: in
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5849 "parser_gen.cpp"
                    break;

                    case 468:  // arrayExps: indexOfArray
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5855 "parser_gen.cpp"
                    break;

                    case 469:  // arrayExps: isArray
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5861 "parser_gen.cpp"
                    break;

                    case 470:  // arrayElemAt: "object" ARRAY_ELEM_AT exprFixedTwoArg "end of
                               // object"
#line 1991 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::arrayElemAt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5870 "parser_gen.cpp"
                    break;

                    case 471:  // arrayToObject: "object" ARRAY_TO_OBJECT expression "end of object"
#line 1998 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::arrayToObject, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5878 "parser_gen.cpp"
                    break;

                    case 472:  // concatArrays: "object" CONCAT_ARRAYS "array" expressions "end of
                               // array" "end of object"
#line 2004 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concatArrays,
                             CNode{YY_MOVE(yystack_[2].value.as<std::vector<CNode>>())}}}};
                    }
#line 5887 "parser_gen.cpp"
                    break;

                    case 473:  // asArg: %empty
#line 2011 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::asArg, CNode{KeyValue::absentKey}};
                    }
#line 5895 "parser_gen.cpp"
                    break;

                    case 474:  // asArg: "as argument" string
#line 2014 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::asArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5903 "parser_gen.cpp"
                    break;

                    case 475:  // filter: "object" FILTER START_ORDERED_OBJECT asArg "cond argument"
                               // expression "input argument" expression "end of object" "end of
                               // object"
#line 2020 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::filter,
                             CNode{CNode::ObjectChildren{
                                 YY_MOVE(
                                     yystack_[6].value.as<std::pair<CNode::Fieldname, CNode>>()),
                                 {KeyFieldname::condArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::inputArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 5912 "parser_gen.cpp"
                    break;

                    case 476:  // first: "object" FIRST expression "end of object"
#line 2027 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::first, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5920 "parser_gen.cpp"
                    break;

                    case 477:  // in: "object" IN exprFixedTwoArg "end of object"
#line 2033 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::in, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5929 "parser_gen.cpp"
                    break;

                    case 478:  // indexOfArray: "object" INDEX_OF_ARRAY "array" expression
                               // expression expression expression "end of array" "end of object"
#line 2041 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfArray,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[5].value.as<CNode>()),
                                                        YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5938 "parser_gen.cpp"
                    break;

                    case 479:  // indexOfArray: "object" INDEX_OF_ARRAY exprFixedTwoArg "end of
                               // object"
#line 2045 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfArray, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5947 "parser_gen.cpp"
                    break;

                    case 480:  // indexOfArray: "object" INDEX_OF_ARRAY exprFixedThreeArg "end of
                               // object"
#line 2049 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::indexOfArray, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5956 "parser_gen.cpp"
                    break;

                    case 481:  // isArray: "object" IS_ARRAY singleArgExpression "end of object"
#line 2056 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isArray, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5964 "parser_gen.cpp"
                    break;

                    case 482:  // stringExps: concat
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5970 "parser_gen.cpp"
                    break;

                    case 483:  // stringExps: dateFromString
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5976 "parser_gen.cpp"
                    break;

                    case 484:  // stringExps: dateToString
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5982 "parser_gen.cpp"
                    break;

                    case 485:  // stringExps: indexOfBytes
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5988 "parser_gen.cpp"
                    break;

                    case 486:  // stringExps: indexOfCP
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5994 "parser_gen.cpp"
                    break;

                    case 487:  // stringExps: ltrim
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6000 "parser_gen.cpp"
                    break;

                    case 488:  // stringExps: regexFind
#line 2062 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6006 "parser_gen.cpp"
                    break;

                    case 489:  // stringExps: regexFindAll
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6012 "parser_gen.cpp"
                    break;

                    case 490:  // stringExps: regexMatch
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6018 "parser_gen.cpp"
                    break;

                    case 491:  // stringExps: replaceOne
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6024 "parser_gen.cpp"
                    break;

                    case 492:  // stringExps: replaceAll
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6030 "parser_gen.cpp"
                    break;

                    case 493:  // stringExps: rtrim
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6036 "parser_gen.cpp"
                    break;

                    case 494:  // stringExps: split
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6042 "parser_gen.cpp"
                    break;

                    case 495:  // stringExps: strLenBytes
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6048 "parser_gen.cpp"
                    break;

                    case 496:  // stringExps: strLenCP
#line 2063 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6054 "parser_gen.cpp"
                    break;

                    case 497:  // stringExps: strcasecmp
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6060 "parser_gen.cpp"
                    break;

                    case 498:  // stringExps: substr
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6066 "parser_gen.cpp"
                    break;

                    case 499:  // stringExps: substrBytes
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6072 "parser_gen.cpp"
                    break;

                    case 500:  // stringExps: substrCP
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6078 "parser_gen.cpp"
                    break;

                    case 501:  // stringExps: toLower
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6084 "parser_gen.cpp"
                    break;

                    case 502:  // stringExps: trim
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6090 "parser_gen.cpp"
                    break;

                    case 503:  // stringExps: toUpper
#line 2064 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6096 "parser_gen.cpp"
                    break;

                    case 504:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 2068 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat,
                             CNode{YY_MOVE(yystack_[2].value.as<std::vector<CNode>>())}}}};
                    }
#line 6105 "parser_gen.cpp"
                    break;

                    case 505:  // formatArg: %empty
#line 2075 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 6113 "parser_gen.cpp"
                    break;

                    case 506:  // formatArg: "format argument" expression
#line 2078 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6121 "parser_gen.cpp"
                    break;

                    case 507:  // timezoneArg: %empty
#line 2084 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 6129 "parser_gen.cpp"
                    break;

                    case 508:  // timezoneArg: "timezone argument" expression
#line 2087 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6137 "parser_gen.cpp"
                    break;

                    case 509:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 2095 "grammar.yy"
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
#line 6147 "parser_gen.cpp"
                    break;

                    case 510:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 2104 "grammar.yy"
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
#line 6157 "parser_gen.cpp"
                    break;

                    case 511:  // dateExps: dateFromParts
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6163 "parser_gen.cpp"
                    break;

                    case 512:  // dateExps: dateToParts
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6169 "parser_gen.cpp"
                    break;

                    case 513:  // dateExps: dayOfMonth
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6175 "parser_gen.cpp"
                    break;

                    case 514:  // dateExps: dayOfWeek
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6181 "parser_gen.cpp"
                    break;

                    case 515:  // dateExps: dayOfYear
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6187 "parser_gen.cpp"
                    break;

                    case 516:  // dateExps: hour
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6193 "parser_gen.cpp"
                    break;

                    case 517:  // dateExps: isoDayOfWeek
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6199 "parser_gen.cpp"
                    break;

                    case 518:  // dateExps: isoWeek
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6205 "parser_gen.cpp"
                    break;

                    case 519:  // dateExps: isoWeekYear
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6211 "parser_gen.cpp"
                    break;

                    case 520:  // dateExps: millisecond
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6217 "parser_gen.cpp"
                    break;

                    case 521:  // dateExps: minute
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6223 "parser_gen.cpp"
                    break;

                    case 522:  // dateExps: month
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6229 "parser_gen.cpp"
                    break;

                    case 523:  // dateExps: second
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6235 "parser_gen.cpp"
                    break;

                    case 524:  // dateExps: week
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6241 "parser_gen.cpp"
                    break;

                    case 525:  // dateExps: year
#line 2113 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6247 "parser_gen.cpp"
                    break;

                    case 526:  // hourArg: %empty
#line 2117 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
                    }
#line 6255 "parser_gen.cpp"
                    break;

                    case 527:  // hourArg: "hour argument" expression
#line 2120 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::hourArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6263 "parser_gen.cpp"
                    break;

                    case 528:  // minuteArg: %empty
#line 2126 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
                    }
#line 6271 "parser_gen.cpp"
                    break;

                    case 529:  // minuteArg: "minute argument" expression
#line 2129 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::minuteArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6279 "parser_gen.cpp"
                    break;

                    case 530:  // secondArg: %empty
#line 2135 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
                    }
#line 6287 "parser_gen.cpp"
                    break;

                    case 531:  // secondArg: "second argument" expression
#line 2138 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::secondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6295 "parser_gen.cpp"
                    break;

                    case 532:  // millisecondArg: %empty
#line 2144 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
                    }
#line 6303 "parser_gen.cpp"
                    break;

                    case 533:  // millisecondArg: "millisecond argument" expression
#line 2147 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::millisecondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6311 "parser_gen.cpp"
                    break;

                    case 534:  // dayArg: %empty
#line 2153 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
                    }
#line 6319 "parser_gen.cpp"
                    break;

                    case 535:  // dayArg: "day argument" expression
#line 2156 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6327 "parser_gen.cpp"
                    break;

                    case 536:  // isoDayOfWeekArg: %empty
#line 2162 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 6335 "parser_gen.cpp"
                    break;

                    case 537:  // isoDayOfWeekArg: "ISO day of week argument" expression
#line 2165 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoDayOfWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6343 "parser_gen.cpp"
                    break;

                    case 538:  // isoWeekArg: %empty
#line 2171 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 6351 "parser_gen.cpp"
                    break;

                    case 539:  // isoWeekArg: "ISO week argument" expression
#line 2174 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6359 "parser_gen.cpp"
                    break;

                    case 540:  // iso8601Arg: %empty
#line 2180 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
                    }
#line 6367 "parser_gen.cpp"
                    break;

                    case 541:  // iso8601Arg: "ISO 8601 argument" bool
#line 2183 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::iso8601Arg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6375 "parser_gen.cpp"
                    break;

                    case 542:  // monthArg: %empty
#line 2189 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
                    }
#line 6383 "parser_gen.cpp"
                    break;

                    case 543:  // monthArg: "month argument" expression
#line 2192 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::monthArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6391 "parser_gen.cpp"
                    break;

                    case 544:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg millisecondArg minuteArg monthArg secondArg
                               // timezoneArg "year argument" expression "end of object" "end of
                               // object"
#line 2199 "grammar.yy"
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
#line 6401 "parser_gen.cpp"
                    break;

                    case 545:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg isoDayOfWeekArg isoWeekArg "ISO week year argument"
                               // expression millisecondArg minuteArg monthArg secondArg timezoneArg
                               // "end of object" "end of object"
#line 2205 "grammar.yy"
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
#line 6411 "parser_gen.cpp"
                    break;

                    case 546:  // dateToParts: "object" DATE_TO_PARTS START_ORDERED_OBJECT "date
                               // argument" expression iso8601Arg timezoneArg "end of object" "end
                               // of object"
#line 2213 "grammar.yy"
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
#line 6421 "parser_gen.cpp"
                    break;

                    case 547:  // dayOfMonth: "object" DAY_OF_MONTH nonArrayNonObjExpression "end of
                               // object"
#line 2221 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6429 "parser_gen.cpp"
                    break;

                    case 548:  // dayOfMonth: "object" DAY_OF_MONTH START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2224 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6438 "parser_gen.cpp"
                    break;

                    case 549:  // dayOfMonth: "object" DAY_OF_MONTH expressionSingletonArray "end of
                               // object"
#line 2228 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6446 "parser_gen.cpp"
                    break;

                    case 550:  // dayOfWeek: "object" DAY_OF_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 2234 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6455 "parser_gen.cpp"
                    break;

                    case 551:  // dayOfWeek: "object" DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2238 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6464 "parser_gen.cpp"
                    break;

                    case 552:  // dayOfWeek: "object" DAY_OF_WEEK expressionSingletonArray "end of
                               // object"
#line 2242 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6472 "parser_gen.cpp"
                    break;

                    case 553:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK nonArrayNonObjExpression
                               // "end of object"
#line 2248 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6481 "parser_gen.cpp"
                    break;

                    case 554:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2252 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6490 "parser_gen.cpp"
                    break;

                    case 555:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK expressionSingletonArray
                               // "end of object"
#line 2256 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6498 "parser_gen.cpp"
                    break;

                    case 556:  // dayOfYear: "object" DAY_OF_YEAR nonArrayNonObjExpression "end of
                               // object"
#line 2262 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6507 "parser_gen.cpp"
                    break;

                    case 557:  // dayOfYear: "object" DAY_OF_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2266 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6516 "parser_gen.cpp"
                    break;

                    case 558:  // dayOfYear: "object" DAY_OF_YEAR expressionSingletonArray "end of
                               // object"
#line 2270 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6524 "parser_gen.cpp"
                    break;

                    case 559:  // hour: "object" HOUR nonArrayNonObjExpression "end of object"
#line 2276 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6533 "parser_gen.cpp"
                    break;

                    case 560:  // hour: "object" HOUR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2280 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6542 "parser_gen.cpp"
                    break;

                    case 561:  // hour: "object" HOUR expressionSingletonArray "end of object"
#line 2284 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6550 "parser_gen.cpp"
                    break;

                    case 562:  // month: "object" MONTH nonArrayNonObjExpression "end of object"
#line 2290 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6559 "parser_gen.cpp"
                    break;

                    case 563:  // month: "object" MONTH START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2294 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6568 "parser_gen.cpp"
                    break;

                    case 564:  // month: "object" MONTH expressionSingletonArray "end of object"
#line 2298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6576 "parser_gen.cpp"
                    break;

                    case 565:  // week: "object" WEEK nonArrayNonObjExpression "end of object"
#line 2304 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6585 "parser_gen.cpp"
                    break;

                    case 566:  // week: "object" WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2308 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6594 "parser_gen.cpp"
                    break;

                    case 567:  // week: "object" WEEK expressionSingletonArray "end of object"
#line 2312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6602 "parser_gen.cpp"
                    break;

                    case 568:  // isoWeek: "object" ISO_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 2318 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6611 "parser_gen.cpp"
                    break;

                    case 569:  // isoWeek: "object" ISO_WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2322 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6620 "parser_gen.cpp"
                    break;

                    case 570:  // isoWeek: "object" ISO_WEEK expressionSingletonArray "end of
                               // object"
#line 2326 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6628 "parser_gen.cpp"
                    break;

                    case 571:  // isoWeekYear: "object" ISO_WEEK_YEAR nonArrayNonObjExpression "end
                               // of object"
#line 2332 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6637 "parser_gen.cpp"
                    break;

                    case 572:  // isoWeekYear: "object" ISO_WEEK_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2336 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6646 "parser_gen.cpp"
                    break;

                    case 573:  // isoWeekYear: "object" ISO_WEEK_YEAR expressionSingletonArray "end
                               // of object"
#line 2340 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6654 "parser_gen.cpp"
                    break;

                    case 574:  // year: "object" YEAR nonArrayNonObjExpression "end of object"
#line 2346 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6663 "parser_gen.cpp"
                    break;

                    case 575:  // year: "object" YEAR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6672 "parser_gen.cpp"
                    break;

                    case 576:  // year: "object" YEAR expressionSingletonArray "end of object"
#line 2354 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6680 "parser_gen.cpp"
                    break;

                    case 577:  // second: "object" SECOND nonArrayNonObjExpression "end of object"
#line 2360 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6689 "parser_gen.cpp"
                    break;

                    case 578:  // second: "object" SECOND START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2364 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6698 "parser_gen.cpp"
                    break;

                    case 579:  // second: "object" SECOND expressionSingletonArray "end of object"
#line 2368 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6706 "parser_gen.cpp"
                    break;

                    case 580:  // millisecond: "object" MILLISECOND nonArrayNonObjExpression "end of
                               // object"
#line 2374 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6715 "parser_gen.cpp"
                    break;

                    case 581:  // millisecond: "object" MILLISECOND START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2378 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6724 "parser_gen.cpp"
                    break;

                    case 582:  // millisecond: "object" MILLISECOND expressionSingletonArray "end of
                               // object"
#line 2382 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6732 "parser_gen.cpp"
                    break;

                    case 583:  // minute: "object" MINUTE nonArrayNonObjExpression "end of object"
#line 2388 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6741 "parser_gen.cpp"
                    break;

                    case 584:  // minute: "object" MINUTE START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2392 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6750 "parser_gen.cpp"
                    break;

                    case 585:  // minute: "object" MINUTE expressionSingletonArray "end of object"
#line 2396 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6758 "parser_gen.cpp"
                    break;

                    case 586:  // exprZeroToTwo: %empty
#line 2402 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 6766 "parser_gen.cpp"
                    break;

                    case 587:  // exprZeroToTwo: expression
#line 2405 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6774 "parser_gen.cpp"
                    break;

                    case 588:  // exprZeroToTwo: expression expression
#line 2408 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6782 "parser_gen.cpp"
                    break;

                    case 589:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 2415 "grammar.yy"
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
#line 6794 "parser_gen.cpp"
                    break;

                    case 590:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 2426 "grammar.yy"
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
#line 6806 "parser_gen.cpp"
                    break;

                    case 591:  // charsArg: %empty
#line 2436 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 6814 "parser_gen.cpp"
                    break;

                    case 592:  // charsArg: "chars argument" expression
#line 2439 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6822 "parser_gen.cpp"
                    break;

                    case 593:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6832 "parser_gen.cpp"
                    break;

                    case 594:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2453 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6842 "parser_gen.cpp"
                    break;

                    case 595:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 2461 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6852 "parser_gen.cpp"
                    break;

                    case 596:  // optionsArg: %empty
#line 2469 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 6860 "parser_gen.cpp"
                    break;

                    case 597:  // optionsArg: "options argument" expression
#line 2472 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6868 "parser_gen.cpp"
                    break;

                    case 598:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 2477 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 6880 "parser_gen.cpp"
                    break;

                    case 599:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 2486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6888 "parser_gen.cpp"
                    break;

                    case 600:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 2492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6896 "parser_gen.cpp"
                    break;

                    case 601:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 2498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6904 "parser_gen.cpp"
                    break;

                    case 602:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2505 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6915 "parser_gen.cpp"
                    break;

                    case 603:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2515 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6926 "parser_gen.cpp"
                    break;

                    case 604:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 2524 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6935 "parser_gen.cpp"
                    break;

                    case 605:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 2531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6944 "parser_gen.cpp"
                    break;

                    case 606:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 2538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6953 "parser_gen.cpp"
                    break;

                    case 607:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 2546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6962 "parser_gen.cpp"
                    break;

                    case 608:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 2554 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6971 "parser_gen.cpp"
                    break;

                    case 609:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 2562 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6980 "parser_gen.cpp"
                    break;

                    case 610:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 2570 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6989 "parser_gen.cpp"
                    break;

                    case 611:  // toLower: "object" TO_LOWER expression "end of object"
#line 2577 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6997 "parser_gen.cpp"
                    break;

                    case 612:  // toUpper: "object" TO_UPPER expression "end of object"
#line 2583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7005 "parser_gen.cpp"
                    break;

                    case 613:  // metaSortKeyword: "randVal"
#line 2589 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 7013 "parser_gen.cpp"
                    break;

                    case 614:  // metaSortKeyword: "textScore"
#line 2592 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 7021 "parser_gen.cpp"
                    break;

                    case 615:  // metaSort: "object" META metaSortKeyword "end of object"
#line 2598 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7029 "parser_gen.cpp"
                    break;

                    case 616:  // sortSpecs: "object" specList "end of object"
#line 2604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 7037 "parser_gen.cpp"
                    break;

                    case 617:  // specList: %empty
#line 2609 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7045 "parser_gen.cpp"
                    break;

                    case 618:  // specList: specList sortSpec
#line 2612 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7054 "parser_gen.cpp"
                    break;

                    case 619:  // oneOrNegOne: "1 (int)"
#line 2619 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 7062 "parser_gen.cpp"
                    break;

                    case 620:  // oneOrNegOne: "-1 (int)"
#line 2622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 7070 "parser_gen.cpp"
                    break;

                    case 621:  // oneOrNegOne: "1 (long)"
#line 2625 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 7078 "parser_gen.cpp"
                    break;

                    case 622:  // oneOrNegOne: "-1 (long)"
#line 2628 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 7086 "parser_gen.cpp"
                    break;

                    case 623:  // oneOrNegOne: "1 (double)"
#line 2631 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 7094 "parser_gen.cpp"
                    break;

                    case 624:  // oneOrNegOne: "-1 (double)"
#line 2634 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 7102 "parser_gen.cpp"
                    break;

                    case 625:  // oneOrNegOne: "1 (decimal)"
#line 2637 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 7110 "parser_gen.cpp"
                    break;

                    case 626:  // oneOrNegOne: "-1 (decimal)"
#line 2640 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 7118 "parser_gen.cpp"
                    break;

                    case 627:  // sortFieldname: valueFieldname
#line 2645 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 7126 "parser_gen.cpp"
                    break;

                    case 628:  // sortFieldname: "fieldname containing dotted path"
#line 2647 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 7138 "parser_gen.cpp"
                    break;

                    case 629:  // sortSpec: sortFieldname metaSort
#line 2657 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7146 "parser_gen.cpp"
                    break;

                    case 630:  // sortSpec: sortFieldname oneOrNegOne
#line 2659 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7154 "parser_gen.cpp"
                    break;

                    case 631:  // findProject: "object" findProjectFields "end of object"
#line 2665 "grammar.yy"
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
#line 7175 "parser_gen.cpp"
                    break;

                    case 632:  // findProjectFields: %empty
#line 2684 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7183 "parser_gen.cpp"
                    break;

                    case 633:  // findProjectFields: findProjectFields findProjectField
#line 2687 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7192 "parser_gen.cpp"
                    break;

                    case 634:  // findProjectField: ID topLevelFindProjection
#line 2694 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7200 "parser_gen.cpp"
                    break;

                    case 635:  // findProjectField: projectionFieldname topLevelFindProjection
#line 2697 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7208 "parser_gen.cpp"
                    break;

                    case 636:  // topLevelFindProjection: findProjection
#line 2703 "grammar.yy"
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
#line 7224 "parser_gen.cpp"
                    break;

                    case 637:  // findProjection: projectionCommon
#line 2717 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7230 "parser_gen.cpp"
                    break;

                    case 638:  // findProjection: findProjectionObject
#line 2718 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7236 "parser_gen.cpp"
                    break;

                    case 639:  // findProjection: aggregationOperatorWithoutSlice
#line 2719 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7242 "parser_gen.cpp"
                    break;

                    case 640:  // findProjection: findProjectionSlice
#line 2720 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7248 "parser_gen.cpp"
                    break;

                    case 641:  // findProjection: elemMatch
#line 2721 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7254 "parser_gen.cpp"
                    break;

                    case 642:  // elemMatch: "object" "elemMatch operator" matchExpression "end of
                               // object"
#line 2725 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = {CNode::ObjectChildren{
                            {KeyFieldname::elemMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7262 "parser_gen.cpp"
                    break;

                    case 643:  // findProjectionSlice: "object" "slice" num "end of object"
#line 2731 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7270 "parser_gen.cpp"
                    break;

                    case 644:  // findProjectionSlice: "object" "slice" "array" num num "end of
                               // array" "end of object"
#line 2734 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7279 "parser_gen.cpp"
                    break;

                    case 645:  // findProjectionObject: "object" findProjectionObjectFields "end of
                               // object"
#line 2742 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 7287 "parser_gen.cpp"
                    break;

                    case 646:  // findProjectionObjectFields: findProjectionObjectField
#line 2749 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7296 "parser_gen.cpp"
                    break;

                    case 647:  // findProjectionObjectFields: findProjectionObjectFields
                               // findProjectionObjectField
#line 2753 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7305 "parser_gen.cpp"
                    break;

                    case 648:  // findProjectionObjectField: idAsProjectionPath findProjection
#line 2761 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7313 "parser_gen.cpp"
                    break;

                    case 649:  // findProjectionObjectField: projectionFieldname findProjection
#line 2764 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7321 "parser_gen.cpp"
                    break;

                    case 650:  // setExpression: allElementsTrue
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7327 "parser_gen.cpp"
                    break;

                    case 651:  // setExpression: anyElementTrue
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7333 "parser_gen.cpp"
                    break;

                    case 652:  // setExpression: setDifference
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7339 "parser_gen.cpp"
                    break;

                    case 653:  // setExpression: setEquals
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7345 "parser_gen.cpp"
                    break;

                    case 654:  // setExpression: setIntersection
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7351 "parser_gen.cpp"
                    break;

                    case 655:  // setExpression: setIsSubset
#line 2770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7357 "parser_gen.cpp"
                    break;

                    case 656:  // setExpression: setUnion
#line 2771 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7363 "parser_gen.cpp"
                    break;

                    case 657:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 2775 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 7371 "parser_gen.cpp"
                    break;

                    case 658:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 2781 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 7379 "parser_gen.cpp"
                    break;

                    case 659:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 2787 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7388 "parser_gen.cpp"
                    break;

                    case 660:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2795 "grammar.yy"
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
#line 7400 "parser_gen.cpp"
                    break;

                    case 661:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 2806 "grammar.yy"
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
#line 7412 "parser_gen.cpp"
                    break;

                    case 662:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 2816 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7421 "parser_gen.cpp"
                    break;

                    case 663:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2824 "grammar.yy"
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
#line 7433 "parser_gen.cpp"
                    break;

                    case 664:  // literalEscapes: const
#line 2834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7439 "parser_gen.cpp"
                    break;

                    case 665:  // literalEscapes: literal
#line 2834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7445 "parser_gen.cpp"
                    break;

                    case 666:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 2838 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7454 "parser_gen.cpp"
                    break;

                    case 667:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 2845 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 7463 "parser_gen.cpp"
                    break;

                    case 668:  // value: simpleValue
#line 2852 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7469 "parser_gen.cpp"
                    break;

                    case 669:  // value: compoundValue
#line 2852 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7475 "parser_gen.cpp"
                    break;

                    case 670:  // compoundValue: valueArray
#line 2856 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7481 "parser_gen.cpp"
                    break;

                    case 671:  // compoundValue: valueObject
#line 2856 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7487 "parser_gen.cpp"
                    break;

                    case 672:  // valueArray: "array" values "end of array"
#line 2860 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 7495 "parser_gen.cpp"
                    break;

                    case 673:  // values: %empty
#line 2866 "grammar.yy"
                    {
                    }
#line 7501 "parser_gen.cpp"
                    break;

                    case 674:  // values: values value
#line 2867 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 7510 "parser_gen.cpp"
                    break;

                    case 675:  // valueObject: "object" valueFields "end of object"
#line 2874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 7518 "parser_gen.cpp"
                    break;

                    case 676:  // valueFields: %empty
#line 2880 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7526 "parser_gen.cpp"
                    break;

                    case 677:  // valueFields: valueFields valueField
#line 2883 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7535 "parser_gen.cpp"
                    break;

                    case 678:  // valueField: valueFieldname value
#line 2890 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7543 "parser_gen.cpp"
                    break;

                    case 679:  // valueFieldname: invariableUserFieldname
#line 2897 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7549 "parser_gen.cpp"
                    break;

                    case 680:  // valueFieldname: stageAsUserFieldname
#line 2898 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7555 "parser_gen.cpp"
                    break;

                    case 681:  // valueFieldname: argAsUserFieldname
#line 2899 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7561 "parser_gen.cpp"
                    break;

                    case 682:  // valueFieldname: aggExprAsUserFieldname
#line 2900 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7567 "parser_gen.cpp"
                    break;

                    case 683:  // valueFieldname: idAsUserFieldname
#line 2901 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7573 "parser_gen.cpp"
                    break;

                    case 684:  // valueFieldname: elemMatchAsUserFieldname
#line 2902 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7579 "parser_gen.cpp"
                    break;

                    case 685:  // compExprs: cmp
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7585 "parser_gen.cpp"
                    break;

                    case 686:  // compExprs: eq
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7591 "parser_gen.cpp"
                    break;

                    case 687:  // compExprs: gt
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7597 "parser_gen.cpp"
                    break;

                    case 688:  // compExprs: gte
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7603 "parser_gen.cpp"
                    break;

                    case 689:  // compExprs: lt
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7609 "parser_gen.cpp"
                    break;

                    case 690:  // compExprs: lte
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7615 "parser_gen.cpp"
                    break;

                    case 691:  // compExprs: ne
#line 2905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7621 "parser_gen.cpp"
                    break;

                    case 692:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 2907 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7630 "parser_gen.cpp"
                    break;

                    case 693:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 2912 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7639 "parser_gen.cpp"
                    break;

                    case 694:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 2917 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7648 "parser_gen.cpp"
                    break;

                    case 695:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 2922 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7657 "parser_gen.cpp"
                    break;

                    case 696:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 2927 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7666 "parser_gen.cpp"
                    break;

                    case 697:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 2932 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7675 "parser_gen.cpp"
                    break;

                    case 698:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 2937 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7684 "parser_gen.cpp"
                    break;

                    case 699:  // typeExpression: convert
#line 2943 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7690 "parser_gen.cpp"
                    break;

                    case 700:  // typeExpression: toBool
#line 2944 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7696 "parser_gen.cpp"
                    break;

                    case 701:  // typeExpression: toDate
#line 2945 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7702 "parser_gen.cpp"
                    break;

                    case 702:  // typeExpression: toDecimal
#line 2946 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7708 "parser_gen.cpp"
                    break;

                    case 703:  // typeExpression: toDouble
#line 2947 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7714 "parser_gen.cpp"
                    break;

                    case 704:  // typeExpression: toInt
#line 2948 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7720 "parser_gen.cpp"
                    break;

                    case 705:  // typeExpression: toLong
#line 2949 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7726 "parser_gen.cpp"
                    break;

                    case 706:  // typeExpression: toObjectId
#line 2950 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7732 "parser_gen.cpp"
                    break;

                    case 707:  // typeExpression: toString
#line 2951 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7738 "parser_gen.cpp"
                    break;

                    case 708:  // typeExpression: type
#line 2952 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7744 "parser_gen.cpp"
                    break;

                    case 709:  // onErrorArg: %empty
#line 2957 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 7752 "parser_gen.cpp"
                    break;

                    case 710:  // onErrorArg: "onError argument" expression
#line 2960 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7760 "parser_gen.cpp"
                    break;

                    case 711:  // onNullArg: %empty
#line 2967 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 7768 "parser_gen.cpp"
                    break;

                    case 712:  // onNullArg: "onNull argument" expression
#line 2970 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7776 "parser_gen.cpp"
                    break;

                    case 713:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2977 "grammar.yy"
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
#line 7787 "parser_gen.cpp"
                    break;

                    case 714:  // toBool: "object" TO_BOOL expression "end of object"
#line 2986 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7795 "parser_gen.cpp"
                    break;

                    case 715:  // toDate: "object" TO_DATE expression "end of object"
#line 2991 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7803 "parser_gen.cpp"
                    break;

                    case 716:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2996 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7811 "parser_gen.cpp"
                    break;

                    case 717:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 3001 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7819 "parser_gen.cpp"
                    break;

                    case 718:  // toInt: "object" TO_INT expression "end of object"
#line 3006 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7827 "parser_gen.cpp"
                    break;

                    case 719:  // toLong: "object" TO_LONG expression "end of object"
#line 3011 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7835 "parser_gen.cpp"
                    break;

                    case 720:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 3016 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7843 "parser_gen.cpp"
                    break;

                    case 721:  // toString: "object" TO_STRING expression "end of object"
#line 3021 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7851 "parser_gen.cpp"
                    break;

                    case 722:  // type: "object" TYPE expression "end of object"
#line 3026 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7859 "parser_gen.cpp"
                    break;


#line 7863 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -1153;

const short ParserGen::yytable_ninf_ = -537;

const short ParserGen::yypact_[] = {
    -66,   -124,  -120,  -107,  -105,  62,    -93,   -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -50,   -7,    319,   2268,  1355,  -89,   637,   -120,  -83,   -79,   637,
    -78,   3,     -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, 4197,  -1153, 4355,  -1153, -1153, -1153, -78,   41,
    -1153, -1153, -1153, -1153, 4829,  -1153, -1153, -1153, -1153, -1153, -67,   -1153, -1153,
    -1153, -1153, 4987,  -1153, -1153, 4987,  -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    296,   -1153, -1153, -1153, -1153, 8,     -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, 55,    -1153, -1153, 83,    -93,   -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, 2094,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, 102,   -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, 1540,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -2,    -1153,
    -1153, -1153, 2339,  637,   135,   -1153, 2617,  1910,  2775,  4513,  4513,  4513,  -42,
    -33,   -42,   -17,   -6,    4355,  4513,  4513,  4513,  -6,    4513,  4513,  -6,    1,
    2,     6,     -78,   4513,  4513,  -78,   -78,   -78,   -78,   4671,  4671,  4671,  4513,
    19,    -6,    4513,  -78,   4355,  4513,  -6,    -6,    4671,  -6,    25,    28,    29,
    4513,  4671,  4671,  4671,  32,    4513,  35,    4513,  -6,    -6,    -78,   5,     4671,
    4671,  36,    4671,  44,    -6,    46,    -42,   47,    4513,  -78,   -78,   -78,   -78,
    -78,   49,    -78,   4671,  -6,    50,    52,    -6,    60,    4513,  4513,  64,    67,
    4513,  70,    4355,  4355,  74,    75,    76,    79,    4513,  4513,  4355,  4355,  4355,
    4355,  4355,  4355,  4355,  4355,  4355,  4355,  -78,   80,    4355,  4671,  4671,  2618,
    7,     101,   -29,   -120,  -120,  -1153, 210,   4987,  4987,  2443,  -1153, -99,   -1153,
    5145,  5145,  -1153, -1153, 58,    88,    -1153, -1153, -1153, 4197,  -1153, -1153, -1153,
    4355,  -1153, -1153, -1153, -1153, -1153, -1153, 69,    77,    81,    82,    4355,  85,
    4355,  4355,  86,    87,    96,    116,   132,   133,   140,   159,   160,   -1153, -1153,
    4197,  124,   162,   164,   143,   136,   150,   227,   2094,  -1153, -1153, 166,   167,
    230,   171,   172,   236,   175,   176,   239,   178,   4355,  179,   180,   251,   186,
    187,   188,   189,   190,   191,   256,   195,   4355,  198,   199,   4355,  4355,  204,
    208,   209,   272,   211,   214,   277,   217,   218,   281,   4197,  220,   4355,  221,
    222,   223,   289,   226,   228,   232,   233,   234,   235,   237,   238,   243,   246,
    247,   298,   252,   261,   301,   4355,  290,   293,   313,   4355,  294,   4355,  297,
    4355,  302,   303,   343,   307,   308,   284,   353,   4355,  289,   310,   311,   360,
    312,   4355,  4355,  314,   4355,  320,   321,   4355,  322,   325,   4355,  331,   4355,
    337,   338,   4355,  4355,  4355,  4355,  348,   349,   350,   351,   355,   356,   359,
    363,   365,   368,   369,   370,   289,   4355,  371,   372,   373,   375,   374,   376,
    440,   -1153, 4355,  -1153, -1153, -1153, -1153, -1153, 7,     358,   -1153, 4197,  304,
    -116,  309,   -1153, -1153, -1153, -1153, -1153, 380,   381,   637,   383,   -1153, -1153,
    -1153, -1153, -1153, -1153, 384,   1725,  -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -31,   -1153, 386,   -1153, -1153, -1153, -1153, 389,   -1153, 390,   4355,  -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, 2933,  3091,  391,   4355,  -1153, -1153,
    4355,  435,   4355,  4355,  4355,  -1153, -1153, 4355,  -1153, -1153, 4355,  -1153, -1153,
    4355,  -1153, 4355,  -1153, -1153, 135,   454,   -1153, -1153, -1153, -1153, -1153, -1153,
    4355,  -1153, 4355,  -1153, -1153, 4355,  4355,  -1153, -1153, -1153, 4355,  -1153, -1153,
    4355,  -1153, -1153, 4355,  393,   -1153, 4355,  -1153, -1153, -1153, 4355,  447,   -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, 4355,  -1153, -1153,
    4355,  4355,  -1153, -1153, 4355,  4355,  -1153, 395,   -1153, 4355,  -1153, -1153, 4355,
    -1153, -1153, 4355,  4355,  4355,  449,   -1153, -1153, 4355,  -1153, 4355,  4355,  -1153,
    4355,  -1153, -1153, 4355,  -1153, -1153, 4355,  -1153, 4355,  -1153, -1153, 4355,  4355,
    4355,  4355,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, 450,   4355,  -1153, -1153, -1153, 4355,  -1153, -1153, 4355,  -1153, -1153, 135,
    436,   -1153, 637,   -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, 637,
    -1153, -1153, 5145,  5145,  -1153, 2514,  400,   -1153, 402,   404,   406,   410,   414,
    415,   451,   -1153, 4355,  89,    472,   470,   472,   456,   456,   456,   422,   -1153,
    4355,  456,   3249,  4355,  4355,  456,   456,   456,   425,   429,   -1153, 4355,  456,
    456,   439,   456,   -1153, 442,   459,   482,   513,   516,   468,   4355,  456,   -1153,
    -1153, -1153, 3249,  469,   474,   4355,  4355,  4355,  475,   4355,  478,   456,   456,
    -1153, 135,   480,   637,   -27,   5189,  488,   -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, 4355,  525,   -1153, 4355,  4355,  539,   544,   4355,
    456,   7,     456,   456,   4355,  494,   495,   496,   498,   555,   502,   3407,  4355,
    506,   507,   511,   512,   515,   -1153, 517,   519,   520,   521,   522,   524,   3565,
    -1153, 526,   4355,  557,   4355,  4355,  530,   536,   540,   3723,  3881,  4039,  545,
    543,   546,   551,   553,   556,   561,   567,   568,   581,   585,   -1153, -1153, 588,
    589,   -1153, -1153, 595,   -1153, 4355,  574,   -1153, -1153, 4355,  602,   4355,  646,
    -1153, 451,   -1153, 597,   525,   -1153, 601,   603,   606,   -1153, 4355,  608,   -1153,
    610,   -1153, 611,   612,   614,   616,   617,   -1153, 622,   623,   624,   -1153, 625,
    626,   -1153, -1153, 4355,  642,   670,   -1153, 631,   638,   639,   640,   641,   -1153,
    -1153, 643,   647,   649,   -1153, 650,   -1153, 651,   652,   -1153, -1153, -1153, -1153,
    4355,  -1153, 4355,  675,   -1153, 4355,  525,   653,   655,   -1153, -1153, -1153, 659,
    -1153, 660,   -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, 661,
    4355,  4355,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    663,   -1153, 4355,  456,   690,   664,   -1153, 665,   671,   -1153, -1153, 672,   673,
    674,   -1153, 679,   539,   676,   -1153, -1153, 677,   678,   -1153, 4355,  602,   -1153,
    -1153, -1153, 680,   675,   681,   456,   -1153, 682,   683,   -1153};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   7,   2,   79,  3,   632, 4,   617, 5,   1,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  11,  12,  13,  14,  15,  16,  6,   109, 167,
    169, 149, 138, 170, 148, 145, 159, 168, 152, 146, 154, 141, 163, 160, 161, 162, 166, 158, 156,
    164, 143, 144, 151, 139, 150, 153, 165, 157, 140, 147, 142, 155, 0,   78,  0,   382, 111, 110,
    0,   0,   117, 115, 116, 114, 0,   136, 80,  81,  83,  82,  0,   84,  85,  86,  631, 0,   70,
    72,  0,   71,  137, 633, 195, 263, 266, 171, 249, 173, 250, 270, 271, 262, 265, 264, 172, 267,
    196, 178, 211, 272, 174, 185, 257, 260, 212, 227, 213, 228, 214, 215, 216, 268, 197, 383, 616,
    179, 198, 273, 274, 199, 180, 181, 217, 275, 276, 229, 230, 277, 218, 219, 220, 175, 200, 201,
    202, 182, 183, 231, 232, 221, 222, 203, 223, 204, 184, 177, 176, 205, 269, 233, 234, 235, 237,
    236, 206, 238, 224, 251, 252, 253, 254, 255, 256, 259, 207, 239, 208, 129, 132, 133, 134, 135,
    131, 130, 242, 240, 241, 243, 244, 245, 209, 258, 261, 186, 187, 188, 189, 190, 191, 246, 192,
    193, 248, 247, 210, 194, 225, 226, 628, 680, 681, 682, 679, 0,   683, 684, 627, 618, 0,   318,
    317, 316, 314, 313, 312, 306, 305, 304, 310, 309, 308, 303, 307, 311, 315, 20,  21,  22,  23,
    25,  26,  28,  0,   24,  9,   0,   7,   320, 319, 279, 280, 281, 282, 283, 284, 285, 286, 673,
    676, 287, 278, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 330,
    331, 332, 333, 334, 339, 335, 336, 337, 340, 341, 104, 321, 322, 324, 325, 326, 338, 327, 328,
    329, 668, 669, 670, 671, 323, 342, 376, 344, 118, 355, 346, 345, 356, 364, 385, 357, 456, 457,
    458, 358, 664, 665, 361, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495,
    496, 497, 498, 499, 500, 501, 503, 502, 359, 685, 686, 687, 688, 689, 690, 691, 365, 511, 512,
    513, 514, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524, 525, 360, 699, 700, 701, 702, 703,
    704, 705, 706, 707, 708, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399,
    400, 366, 462, 463, 464, 465, 466, 467, 468, 469, 362, 650, 651, 652, 653, 654, 655, 656, 363,
    410, 411, 412, 413, 414, 415, 416, 417, 418, 420, 421, 422, 419, 423, 424, 347, 354, 120, 128,
    127, 90,  88,  87,  112, 64,  63,  60,  59,  62,  56,  55,  58,  48,  47,  50,  52,  51,  54,
    0,   49,  53,  57,  61,  43,  44,  45,  46,  65,  66,  67,  36,  37,  38,  39,  40,  41,  42,
    637, 68,  639, 634, 636, 640, 641, 638, 635, 626, 625, 624, 623, 620, 619, 622, 621, 0,   629,
    630, 18,  0,   0,   0,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   122, 0,   0,   0,   384, 0,   0,   0,   0,   646, 0,   27,  0,   0,   69,
    29,  0,   0,   672, 674, 675, 0,   677, 371, 343, 0,   348, 352, 373, 349, 353, 374, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   342, 342, 0,   0,
    0,   0,   534, 0,   0,   0,   9,   350, 351, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   473, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   591, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   591, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   591, 0,   0,   0,   0,   0,   0,   0,   0,   375, 0,   380, 379,
    381, 377, 121, 0,   124, 89,  0,   0,   0,   0,   91,  92,  95,  93,  94,  113, 0,   0,   0,
    649, 648, 645, 647, 613, 614, 0,   0,   30,  32,  33,  34,  35,  31,  17,  0,   678, 0,   427,
    449, 452, 425, 0,   459, 0,   0,   470, 471, 448, 451, 450, 426, 453, 428, 692, 0,   0,   0,
    0,   443, 446, 0,   526, 0,   0,   0,   549, 547, 0,   552, 550, 0,   558, 556, 0,   454, 0,
    693, 430, 0,   0,   476, 431, 694, 695, 561, 559, 0,   477, 0,   479, 480, 0,   0,   481, 555,
    553, 0,   570, 568, 0,   573, 571, 0,   0,   432, 0,   434, 696, 697, 0,   0,   401, 402, 403,
    404, 405, 406, 407, 408, 409, 582, 580, 0,   585, 583, 0,   0,   564, 562, 0,   0,   698, 0,
    460, 0,   455, 599, 0,   600, 601, 0,   0,   0,   0,   579, 577, 0,   659, 0,   0,   662, 0,
    442, 445, 0,   369, 370, 0,   439, 0,   605, 606, 0,   0,   0,   0,   444, 447, 714, 715, 716,
    717, 718, 719, 611, 720, 721, 612, 0,   0,   722, 567, 565, 0,   576, 574, 0,   378, 123, 0,
    0,   96,  0,   90,  105, 98,  101, 103, 102, 100, 108, 642, 0,   643, 615, 0,   0,   74,  0,
    0,   372, 0,   0,   0,   0,   0,   0,   709, 535, 0,   532, 505, 540, 505, 507, 507, 507, 0,
    474, 0,   507, 0,   586, 586, 507, 507, 507, 0,   0,   592, 0,   507, 507, 0,   507, 342, 0,
    0,   596, 0,   0,   0,   0,   507, 342, 342, 342, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    507, 507, 125, 0,   0,   0,   0,   0,   0,   77,  76,  73,  75,  19,  657, 658, 367, 504, 472,
    666, 0,   711, 527, 0,   0,   528, 538, 0,   507, 0,   507, 507, 0,   0,   0,   0,   0,   0,
    0,   0,   587, 0,   0,   0,   0,   0,   667, 0,   0,   0,   0,   0,   0,   0,   461, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   126, 119, 0,   91,  97,  99,  0,   710, 0,   0,   537, 533, 0,   542, 0,   0,   506, 709,
    541, 0,   711, 508, 0,   0,   0,   429, 0,   0,   368, 0,   588, 0,   0,   0,   0,   0,   433,
    0,   0,   0,   435, 0,   0,   437, 597, 0,   0,   0,   438, 0,   0,   0,   0,   0,   604, 607,
    0,   0,   0,   440, 0,   441, 0,   0,   107, 106, 644, 712, 0,   529, 0,   530, 539, 0,   711,
    0,   0,   548, 551, 557, 0,   560, 0,   589, 590, 554, 569, 572, 593, 581, 584, 563, 436, 0,
    0,   0,   594, 578, 660, 661, 663, 608, 609, 610, 595, 566, 575, 0,   543, 0,   507, 532, 0,
    546, 0,   0,   478, 598, 0,   0,   0,   531, 0,   528, 0,   510, 475, 0,   0,   713, 0,   542,
    509, 603, 602, 0,   530, 0,   507, 544, 0,   0,   545};

const short ParserGen::yypgoto_[] = {
    -1153, 229,   -16,   -1153, -1153, -3,    -1153, -1153, 10,    -1153, 20,    -1153, -806,
    248,   -1153, -1153, -252,  -1153, -1153, -4,    -57,   -70,   -36,   -32,   -15,   -8,
    4,     -9,    9,     14,    18,    -476,  -74,   -1153, 26,    30,    34,    -607,  38,
    54,    -61,   645,   -1153, -1153, -1153, -1153, -1153, -1153, -313,  -1153, 509,   -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, 139,   -955,  -578,  -1153,
    -20,   -72,   361,   182,   -1153, -85,   4851,  -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -514,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -392,  -1152, -1153,
    -254,  -956,  -719,  -1153, -1153, -496,  -508,  -481,  -1153, -1153, -1153, -500,  -1153,
    -621,  -1153, -258,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -333,  -76,   -39,   4896,  538,   -1,    -1153, -217,  -1153, -1153,
    -1153, -1153, -1153, -295,  -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153, -1153,
    -1153, -1153, -1153, -1153, -1153, -1153, -1153, 684,   -545,  -1153, -1153, -1153, -1153,
    -1153, 156,   -1153, -1153, -1153, -1153, -1153, -1153, -1153, 715};

const short ParserGen::yydefgoto_[] = {
    -1,   1003, 626,  795,  217,  218,  97,   219,  220,  221,  222,  223, 619,  224,  82,   627,
    1005, 799,  634,  98,   284,  285,  286,  287,  288,  289,  290,  291, 292,  293,  294,  295,
    296,  297,  298,  299,  300,  301,  302,  303,  304,  312,  306,  307, 308,  501,  309,  995,
    996,  7,    16,   27,   28,   29,   30,   31,   32,   33,   34,   496, 1006, 825,  826,  475,
    828,  997,  636,  652,  698,  314,  315,  316,  610,  317,  318,  319, 320,  321,  322,  323,
    324,  325,  326,  327,  328,  329,  330,  331,  332,  333,  334,  335, 336,  743,  337,  338,
    339,  340,  341,  342,  343,  344,  345,  346,  347,  348,  349,  350, 351,  352,  353,  354,
    355,  356,  357,  358,  359,  360,  361,  362,  363,  364,  365,  366, 367,  368,  369,  370,
    371,  372,  373,  374,  375,  376,  377,  378,  379,  380,  381,  382, 383,  384,  385,  386,
    387,  388,  389,  390,  391,  392,  393,  394,  395,  396,  397,  398, 399,  400,  401,  402,
    403,  404,  405,  406,  407,  408,  1085, 1150, 877,  1092, 1097, 908, 1121, 1018, 1154, 1251,
    1089, 859,  1156, 1094, 1212, 1090, 502,  500,  1105, 409,  410,  411, 412,  413,  414,  415,
    416,  417,  418,  419,  420,  421,  422,  423,  424,  425,  426,  427, 428,  429,  430,  431,
    432,  640,  641,  433,  434,  643,  644,  675,  9,    17,   613,  440, 614,  84,   85,   86,
    808,  809,  810,  811,  812,  87,   88,   1071, 89,   90,   91,   612, 802,  988,  1068, 11,
    18,   478,  479,  480,  481,  482,  620,  99,   621,  13,   19,   493, 494,  823,  225,  5,
    744};

const short ParserGen::yytable_[] = {
    313,  247,  96,   437,  800,  251,  243,  436,  476,  1217, 243,  476,  245,  83,   79,   83,
    245,  477,  1004, 468,  477,  248,  468,  462,  631,  244,  462,  80,   821,  244,  246,  941,
    474,  6,    246,  474,  461,  81,   8,    461,  852,  853,  991,  230,  231,  232,  827,  827,
    1073, 1074, 803,  10,   804,  12,   804,  745,  746,  463,  255,  256,  463,  464,  14,   1253,
    464,  15,   976,  1098, 1099, 226,  822,  35,   1102, 817,  818,  249,  1107, 1108, 1109, 250,
    252,  276,  254,  1113, 1114, 465,  1116, 495,  465,  805,  441,  805,  718,  719,  806,  1126,
    806,  497,  498,  720,  20,   21,   22,   23,   24,   25,   26,   466,  1139, 1140, 466,  467,
    611,  622,  467,  310,  1087, -536, -536, 469,  1088, 801,  469,  470,  648,  832,  470,  471,
    257,  258,  471,  472,  721,  722,  472,  259,  1158, 831,  1160, 1161, 650,  723,  724,  1,
    2,    3,    4,    473,  835,  855,  473,  651,  725,  807,  860,  807,  836,  833,  661,  662,
    837,  838,  858,  663,  840,  843,  844,  861,  260,  261,  642,  642,  642,  241,  726,  845,
    684,  262,  263,  642,  642,  642,  696,  642,  642,  699,  700,  854,  264,  711,  642,  642,
    713,  733,  986,  846,  673,  673,  673,  642,  1004, 737,  642,  739,  741,  642,  749,  755,
    673,  756,  267,  847,  848,  642,  673,  673,  673,  758,  642,  849,  642,  761,  257,  258,
    764,  673,  673,  766,  673,  259,  268,  769,  770,  771,  642,  901,  772,  786,  850,  851,
    278,  856,  673,  857,  862,  863,  864,  865,  642,  642,  866,  867,  642,  868,  869,  870,
    871,  872,  874,  875,  642,  642,  260,  261,  876,  878,  879,  880,  881,  882,  883,  262,
    263,  884,  885,  673,  673,  887,  888,  227,  228,  229,  264,  891,  230,  231,  232,  892,
    893,  894,  895,  829,  829,  896,  897,  1263, 898,  899,  900,  902,  904,  905,  906,  907,
    267,  909,  938,  910,  233,  234,  235,  911,  912,  913,  914,  920,  915,  916,  923,  236,
    237,  238,  917,  1281, 268,  918,  919,  36,   989,  37,   927,  921,  38,   39,   40,   41,
    42,   43,   44,   45,   922,  46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,
    57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   484,  485,  815,
    935,  925,  486,  487,  926,  929,  70,   939,  931,  944,  227,  228,  229,  933,  934,  230,
    231,  232,  936,  937,  987,  942,  943,  945,  981,  948,  488,  489,  257,  258,  71,   950,
    951,  953,  72,   259,  954,  490,  491,  233,  234,  235,  956,  73,   239,  240,  241,  242,
    958,  959,  236,  237,  238,  647,  1117, 649,  629,  827,  827,  964,  965,  966,  967,  1127,
    1128, 1129, 968,  969,  260,  261,  970,  653,  618,  74,   971,  75,   972,  262,  263,  973,
    974,  975,  978,  979,  980,  982,  492,  983,  264,  984,  998,  1017, 999,  990,  1001, 1002,
    1008, 688,  993,  1009, 1010, 1014, 1027, 1035, 1038, 1044, 1050, 1062, 1067, 628,  267,  1077,
    740,  1078, 243,  1079, 1080, 1084, 1159, 76,   245,  1081, 674,  677,  680,  1082, 1083, 1091,
    1093, 83,   268,  1096, 1100, 244,  692,  77,   1110, 78,   246,  1111, 702,  705,  708,  239,
    240,  241,  242,  767,  768,  1115, 1120, 727,  730,  1118, 734,  775,  776,  777,  778,  779,
    780,  781,  782,  783,  784,  476,  476,  787,  751,  1119, 1122, 476,  476,  1123, 477,  477,
    468,  468,  1124, 1131, 462,  462,  468,  468,  1132, 1136, 462,  462,  1138, 474,  474,  1142,
    1149, 461,  461,  474,  474,  834,  1147, 461,  461,  788,  791,  1153, 1155, 1163, 1164, 1165,
    839,  1166, 841,  842,  1167, 1168, 463,  463,  1172, 1173, 464,  464,  463,  463,  1174, 1175,
    464,  464,  1176, 1186, 1177, 816,  1178, 1179, 1180, 1181, 243,  1182, 618,  1184, 83,   796,
    245,  1189, 465,  465,  873,  813,  814,  1190, 465,  465,  1209, 1191, 797,  244,  1195, 1169,
    886,  1196, 246,  889,  890,  1197, 798,  1198, 466,  466,  1199, 1211, 467,  467,  466,  466,
    1200, 903,  467,  467,  469,  469,  1201, 1202, 470,  470,  469,  469,  471,  471,  470,  470,
    472,  472,  471,  471,  1203, 924,  472,  472,  1204, 928,  1205, 930,  1206, 932,  829,  829,
    473,  473,  1207, 1214, 1216, 940,  473,  473,  1218, 1235, 1219, 946,  947,  1220, 949,  1222,
    1223, 952,  1224, 1225, 955,  1226, 957,  1227, 1228, 960,  961,  962,  963,  1229, 1230, 1231,
    1232, 1233, 227,  228,  229,  1236, 1237, 230,  231,  232,  977,  305,  1250, 1238, 1239, 1240,
    1241, 1088, 1242, 985,  1271, 625,  1243, 439,  1244, 1245, 1246, 1247, 1254, 994,  1255, 233,
    234,  235,  1256, 1257, 1258, 253,  1261, 1265, 1266, 992,  236,  237,  238,  633,  1267, 1268,
    1269, 1270, 1076, 1273, 1274, 1275, 1146, 1278, 1280, 1282, 1283, 499,  830,  763,  1215, 1095,
    1272, 1279, 1011, 1264, 1277, 1106, 1070, 1144, 820,  0,    0,    0,    483,  0,    0,    1015,
    0,    0,    1016, 0,    1019, 1020, 1021, 435,  243,  1022, 0,    1000, 1023, 0,    245,  1024,
    243,  1025, 1026, 0,    0,    0,    245,  0,    0,    0,    0,    244,  1028, 0,    1029, 0,
    246,  1030, 1031, 244,  0,    0,    1032, 1007, 246,  1033, 0,    0,    1034, 0,    0,    1036,
    0,    0,    0,    1037, 0,    0,    0,    239,  240,  241,  242,  0,    0,    0,    0,    0,
    1039, 0,    0,    1040, 1041, 0,    0,    1042, 1043, 0,    0,    0,    1045, 0,    0,    1046,
    0,    0,    1047, 1048, 1049, 0,    0,    0,    1051, 0,    1052, 1053, 657,  1054, 0,    660,
    1055, 0,    0,    1056, 0,    1057, 0,    0,    1058, 1059, 1060, 1061, 0,    0,    0,    685,
    0,    0,    0,    0,    690,  691,  0,    695,  697,  1063, 0,    0,    0,    1064, 0,    0,
    1065, 1066, 0,    715,  716,  0,    476,  476,  0,    0,    0,    0,    738,  0,    0,    0,
    0,    468,  468,  0,    0,    462,  462,  0,    0,    754,  0,    0,    757,  0,    474,  474,
    762,  1086, 461,  461,  0,    0,    0,    0,    0,    0,    0,    1101, 0,    1103, 1104, 1104,
    0,    0,    0,    0,    0,    0,    1112, 463,  463,  0,    1069, 464,  464,  0,    0,    243,
    0,    0,    1125, 0,    1072, 245,  0,    1130, 0,    243,  1133, 1134, 1135, 0,    1137, 245,
    0,    1141, 244,  465,  465,  994,  0,    246,  0,    0,    0,    0,    244,  0,    0,    0,
    0,    246,  0,    0,    1148, 0,    0,    1151, 1152, 466,  466,  1157, 0,    467,  467,  0,
    1162, 0,    0,    0,    0,    469,  469,  1170, 1171, 470,  470,  0,    0,    471,  471,  0,
    0,    472,  472,  0,    0,    0,    0,    0,    1185, 1143, 1187, 1188, 0,    0,    243,  0,
    243,  473,  473,  0,    245,  0,    245,  0,    0,    0,    0,    0,    678,  681,  0,    0,
    0,    244,  0,    244,  0,    1208, 246,  693,  246,  1210, 0,    1213, 0,    703,  706,  709,
    0,    0,    0,    0,    0,    0,    0,    1221, 728,  731,  0,    735,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    752,  1234, 0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    1248, 0,    1249, 0,    0,    1252, 0,    0,    305,  789,  792,  638,  638,  638,  0,
    0,    0,    0,    0,    0,    638,  638,  638,  0,    638,  638,  1259, 1260, 0,    0,    0,
    638,  638,  0,    0,    0,    0,    672,  672,  672,  638,  1262, 0,    638,  0,    0,    638,
    0,    0,    672,  0,    0,    0,    0,    638,  672,  672,  672,  0,    638,  0,    638,  1276,
    0,    0,    0,    672,  672,  0,    672,  0,    0,    0,    0,    0,    638,  0,    0,    0,
    0,    0,    0,    0,    672,  0,    0,    0,    0,    0,    638,  638,  0,    0,    638,  0,
    0,    0,    0,    0,    0,    664,  638,  638,  667,  668,  669,  670,  676,  679,  682,  0,
    0,    0,    0,    687,  0,    672,  672,  0,    694,  0,    0,    0,    0,    0,    704,  707,
    710,  0,    0,    0,    0,    0,    0,    717,  0,    729,  732,  0,    736,  0,    305,  0,
    0,    0,    0,    0,    0,    0,    747,  748,  0,    750,  753,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    305,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    785,  0,    0,    790,  793,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    305,  0,    100,  101,
    102,  103,  104,  105,  106,  37,   107,  108,  38,   39,   40,   41,   42,   43,   44,   45,
    0,    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    61,   62,   63,   64,   65,   66,   67,   68,   69,   109,  110,  111,  112,  113,  0,    0,
    114,  115,  0,    116,  117,  118,  119,  120,  121,  122,  123,  124,  125,  126,  127,  128,
    0,    0,    0,    129,  130,  0,    0,    0,    131,  0,    132,  133,  0,    134,  0,    135,
    136,  137,  0,    0,    138,  139,  140,  73,   141,  305,  142,  143,  144,  0,    0,    0,
    145,  146,  147,  148,  149,  150,  151,  152,  0,    0,    0,    153,  154,  155,  156,  157,
    158,  159,  160,  161,  162,  0,    163,  164,  165,  166,  0,    0,    167,  168,  169,  170,
    171,  172,  173,  0,    0,    174,  175,  176,  177,  178,  179,  180,  181,  182,  0,    183,
    184,  185,  186,  187,  188,  189,  190,  191,  0,    0,    192,  193,  194,  195,  196,  197,
    198,  199,  200,  0,    0,    201,  202,  203,  204,  205,  206,  207,  208,  209,  210,  211,
    212,  213,  214,  0,    215,  78,   216,  503,  504,  505,  506,  507,  508,  509,  37,   510,
    511,  38,   39,   40,   41,   42,   43,   44,   45,   0,    46,   47,   48,   49,   50,   51,
    52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,
    68,   69,   512,  513,  514,  515,  516,  0,    0,    517,  518,  0,    519,  520,  521,  522,
    523,  524,  525,  526,  527,  528,  529,  530,  531,  0,    0,    0,    532,  533,  0,    0,
    0,    615,  0,    0,    534,  0,    535,  0,    536,  537,  538,  0,    0,    539,  540,  541,
    616,  542,  0,    543,  544,  545,  0,    0,    0,    546,  547,  548,  549,  550,  551,  552,
    553,  0,    0,    0,    554,  555,  556,  557,  558,  559,  560,  561,  562,  563,  0,    564,
    565,  566,  567,  0,    0,    568,  569,  570,  571,  572,  573,  574,  0,    0,    575,  576,
    577,  578,  579,  580,  581,  582,  617,  0,    584,  585,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    586,  587,  588,  589,  590,  591,  592,  593,  594,  0,    0,    595,  596,
    597,  598,  599,  600,  601,  602,  603,  604,  605,  606,  607,  608,  0,    609,  94,   95,
    503,  504,  505,  506,  507,  508,  509,  37,   510,  511,  38,   39,   40,   41,   42,   43,
    44,   45,   0,    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   512,  513,  514,  515,  516,
    0,    0,    517,  518,  0,    519,  520,  521,  522,  523,  524,  525,  526,  527,  528,  529,
    530,  531,  0,    0,    0,    532,  533,  0,    0,    0,    0,    0,    0,    534,  0,    535,
    0,    536,  537,  538,  0,    0,    539,  540,  541,  616,  542,  0,    543,  544,  545,  0,
    0,    0,    546,  547,  548,  549,  550,  551,  552,  553,  0,    0,    0,    554,  555,  556,
    557,  558,  559,  560,  561,  562,  563,  0,    564,  565,  566,  567,  0,    0,    568,  569,
    570,  571,  572,  573,  574,  0,    0,    575,  576,  577,  578,  579,  580,  581,  582,  583,
    0,    584,  585,  0,    0,    0,    0,    0,    0,    0,    0,    0,    586,  587,  588,  589,
    590,  591,  592,  593,  594,  0,    0,    595,  596,  597,  598,  599,  600,  601,  602,  603,
    604,  605,  606,  607,  608,  0,    609,  94,   95,   100,  101,  102,  103,  104,  105,  106,
    37,   107,  108,  38,   39,   40,   41,   42,   43,   44,   45,   0,    46,   47,   48,   49,
    50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,
    66,   67,   68,   69,   109,  110,  111,  112,  113,  0,    0,    114,  115,  0,    116,  117,
    118,  119,  120,  121,  122,  123,  124,  125,  126,  127,  128,  0,    0,    0,    129,  130,
    0,    0,    0,    131,  0,    632,  133,  0,    134,  0,    135,  136,  137,  0,    0,    138,
    139,  140,  73,   141,  0,    142,  143,  144,  0,    0,    0,    145,  146,  147,  148,  149,
    150,  151,  152,  0,    0,    0,    153,  154,  155,  156,  157,  158,  159,  160,  161,  162,
    0,    163,  164,  165,  166,  0,    0,    167,  168,  169,  170,  171,  172,  173,  0,    0,
    174,  175,  176,  177,  178,  179,  180,  181,  182,  0,    183,  184,  185,  186,  187,  188,
    189,  190,  191,  0,    0,    192,  193,  194,  195,  196,  197,  198,  199,  200,  0,    0,
    201,  202,  203,  204,  205,  206,  207,  208,  209,  210,  211,  212,  213,  214,  0,    215,
    78,   503,  504,  505,  506,  507,  508,  509,  0,    510,  511,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    512,  513,  514,  515,
    516,  0,    0,    517,  518,  0,    519,  520,  521,  522,  523,  524,  525,  526,  527,  528,
    529,  530,  531,  0,    0,    0,    532,  533,  0,    0,    0,    0,    0,    0,    534,  0,
    535,  0,    536,  537,  538,  0,    0,    539,  540,  541,  0,    542,  0,    543,  544,  545,
    0,    0,    0,    546,  547,  548,  549,  550,  551,  552,  553,  0,    0,    0,    554,  555,
    556,  557,  558,  559,  560,  561,  562,  563,  0,    564,  565,  566,  567,  0,    0,    568,
    569,  570,  571,  572,  573,  574,  0,    0,    575,  576,  577,  578,  579,  580,  581,  582,
    583,  0,    584,  585,  0,    0,    0,    0,    0,    0,    0,    0,    0,    586,  587,  588,
    589,  590,  591,  592,  593,  594,  0,    0,    595,  596,  597,  598,  599,  600,  601,  602,
    603,  604,  605,  606,  607,  608,  37,   609,  0,    38,   39,   40,   41,   42,   43,   44,
    45,   0,    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,
    60,   61,   62,   63,   64,   65,   66,   67,   68,   69,   0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    92,   0,    37,   0,    0,
    38,   39,   40,   41,   42,   43,   44,   45,   93,   46,   47,   48,   49,   50,   51,   52,
    53,   54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,
    69,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    623,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    624,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    37,   94,   95,   38,   39,   40,   41,   42,   43,   44,   45,
    0,    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    61,   62,   63,   64,   65,   66,   67,   68,   69,   0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    819,  0,    37,   94,   95,   38,
    39,   40,   41,   42,   43,   44,   45,   616,  46,   47,   48,   49,   50,   51,   52,   53,
    54,   55,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   69,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    1075, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    616,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    37,   94,   95,   38,   39,   40,   41,   42,   43,   44,   45,   0,
    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   61,
    62,   63,   64,   65,   66,   67,   68,   69,   0,    0,    0,    0,    255,  256,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,
    229,  0,    0,    230,  231,  232,  0,    630,  0,    794,  0,    0,    94,   95,   0,    0,
    257,  258,  0,    0,    0,    0,    73,   259,  0,    0,    0,    233,  234,  235,  0,    0,
    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,
    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    265,  266,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    78,   0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,
    278,  279,  280,  239,  240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,
    0,    230,  231,  232,  0,    635,  0,    0,    0,    0,    0,    0,    0,    0,    257,  258,
    0,    0,    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,
    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,
    263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,
    280,  239,  240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,
    231,  232,  0,    1012, 0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,
    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,
    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,
    240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,
    0,    1013, 0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,
    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,
    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,
    242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    1080,
    0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,
    0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,
    238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    310,  311,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,
    270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,
    282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    1169, 0,    0,
    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,
    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,
    0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    310,  311,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,
    272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,  282,  283,
    255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    1183, 0,    0,    0,    0,
    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    233,
    234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,
    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    310,  311,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,
    274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,  282,  283,  255,  256,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    227,  228,  229,  0,    0,    230,  231,  232,  0,    1192, 0,    0,    0,    0,    0,    0,
    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,
    0,    0,    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,
    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    310,  311,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,
    276,  277,  278,  279,  280,  239,  240,  241,  242,  281,  282,  283,  255,  256,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,
    229,  0,    0,    230,  231,  232,  0,    1193, 0,    0,    0,    0,    0,    0,    0,    0,
    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,
    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,
    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,
    278,  279,  280,  239,  240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,
    0,    230,  231,  232,  0,    1194, 0,    0,    0,    0,    0,    0,    0,    0,    257,  258,
    0,    0,    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,
    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,
    263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,
    280,  239,  240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,
    231,  232,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,
    0,    0,    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,
    0,    0,    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    265,  266,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,
    240,  241,  242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,
    0,    259,  0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,
    236,  237,  238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    310,  311,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    268,  269,  270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,
    242,  281,  282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,
    0,    0,    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,
    238,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    637,  311,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,
    270,  271,  272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,
    282,  283,  255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,
    0,    233,  234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,
    0,    0,    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    637,  671,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,
    272,  273,  274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,  282,  283,
    255,  256,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    227,  228,  229,  0,    0,    230,  231,  232,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    233,
    234,  235,  0,    0,    0,    0,    0,    0,    0,    0,    236,  237,  238,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,
    0,    0,    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    264,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    438,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    268,  269,  270,  271,  272,  273,
    274,  275,  276,  277,  278,  279,  280,  239,  240,  241,  242,  281,  282,  283,  442,  443,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    444,  445,  446,  0,    0,    447,  448,  449,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    450,  451,  452,
    0,    0,    0,    0,    0,    0,    0,    0,    453,  454,  455,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    260,  261,  0,    0,    0,    0,
    0,    0,    0,    262,  263,  0,    0,    0,    0,    0,    0,    0,    0,    0,    264,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    310,  456,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    267,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    268,  0,    0,    271,  272,  273,  274,  275,
    276,  277,  278,  279,  280,  457,  458,  459,  460,  281,  282,  283,  442,  443,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    444,  445,
    446,  0,    0,    447,  448,  449,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    257,  258,  0,    0,    0,    0,    0,    259,  0,    0,    0,    450,  451,  452,  0,    0,
    0,    0,    0,    0,    0,    0,    453,  454,  455,  0,    227,  228,  229,  0,    0,    230,
    231,  232,  0,    1145, 0,    0,    0,    0,    260,  261,  0,    0,    257,  258,  0,    0,
    0,    262,  263,  259,  0,    0,    0,    233,  234,  235,  0,    0,    264,  0,    0,    0,
    0,    0,    236,  237,  238,  0,    310,  824,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    267,  0,    260,  261,  0,    0,    0,    0,    0,    0,    0,    262,  263,  0,
    0,    0,    0,    0,    0,    0,    268,  0,    264,  271,  272,  273,  274,  275,  276,  277,
    278,  279,  280,  457,  458,  459,  460,  281,  282,  283,  639,  639,  639,  0,    267,  0,
    0,    0,    0,    639,  639,  639,  0,    639,  639,  0,    0,    0,    0,    0,    639,  639,
    0,    0,    268,  0,    0,    0,    0,    639,  0,    0,    639,  0,    0,    639,  0,    239,
    240,  241,  242,  0,    0,    639,  0,    0,    645,  646,  639,  0,    639,  0,    0,    0,
    654,  655,  656,  0,    658,  659,  0,    0,    0,    0,    639,  665,  666,  0,    0,    0,
    0,    0,    0,    0,    683,  0,    0,    686,  639,  639,  689,  0,    639,  0,    0,    0,
    0,    0,    701,  0,    639,  639,  0,    712,  0,    714,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    742,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    759,  760,  0,    0,    765,  0,    0,    0,    0,    0,    0,
    0,    773,  774};

const short ParserGen::yycheck_[] = {
    72,   21,   18,   77,   611,  25,   21,   77,   93,   1161, 25,   96,   21,   17,   17,   19,
    25,   93,   824,  93,   96,   22,   96,   93,   500,  21,   96,   17,   127,  25,   21,   750,
    93,   157,  25,   96,   93,   17,   158,  96,   661,  662,  158,  74,   75,   76,   624,  625,
    1003, 1004, 79,   158,  81,   158,  81,   569,  570,  93,   51,   52,   96,   93,   0,    1215,
    96,   158,  785,  1023, 1024, 158,  169,  78,   1028, 618,  619,  158,  1032, 1033, 1034, 158,
    158,  197,  79,   1039, 1040, 93,   1042, 79,   96,   118,  157,  118,  87,   88,   123,  1051,
    123,  42,   15,   94,   150,  151,  152,  153,  154,  155,  156,  93,   1064, 1065, 96,   93,
    10,   115,  96,   157,  27,   28,   29,   93,   31,   20,   96,   93,   157,  37,   96,   93,
    87,   88,   96,   93,   127,  128,  96,   94,   1092, 79,   1094, 1095, 157,  136,  137,  209,
    210,  211,  212,  93,   79,   25,   96,   157,  147,  182,  18,   182,  79,   633,  157,  157,
    79,   79,   19,   157,  79,   79,   79,   17,   127,  128,  503,  504,  505,  204,  169,  79,
    157,  136,  137,  512,  513,  514,  157,  516,  517,  157,  157,  663,  147,  157,  523,  524,
    157,  157,  801,  79,   529,  530,  531,  532,  1006, 157,  535,  157,  157,  538,  157,  157,
    541,  157,  169,  79,   79,   546,  547,  548,  549,  157,  551,  79,   553,  157,  87,   88,
    157,  558,  559,  157,  561,  94,   189,  157,  157,  157,  567,  711,  157,  157,  79,   79,
    199,  79,   575,  79,   17,   79,   79,   17,   581,  582,  79,   79,   585,  17,   79,   79,
    17,   79,   79,   79,   593,  594,  127,  128,  13,   79,   79,   79,   79,   79,   79,   136,
    137,  17,   79,   608,  609,  79,   79,   69,   70,   71,   147,  79,   74,   75,   76,   79,
    79,   17,   79,   624,  625,  79,   17,   1251, 79,   79,   17,   79,   79,   79,   79,   14,
    169,  79,   22,   79,   98,   99,   100,  79,   79,   79,   79,   17,   79,   79,   17,   109,
    110,  111,  79,   1279, 189,  79,   79,   8,    804,  10,   17,   79,   13,   14,   15,   16,
    17,   18,   19,   20,   79,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,
    33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   69,   70,   157,
    25,   79,   74,   75,   79,   79,   55,   22,   79,   17,   69,   70,   71,   79,   79,   74,
    75,   76,   79,   79,   30,   79,   79,   79,   17,   79,   98,   99,   87,   88,   79,   79,
    79,   79,   83,   94,   79,   109,  110,  98,   99,   100,  79,   92,   202,  203,  204,  205,
    79,   79,   109,  110,  111,  506,  1043, 508,  498,  1003, 1004, 79,   79,   79,   79,   1052,
    1053, 1054, 79,   79,   127,  128,  79,   511,  456,  122,  79,   124,  79,   136,  137,  79,
    79,   79,   79,   79,   79,   79,   158,  79,   147,  17,   78,   24,   79,   157,  79,   79,
    78,   537,  157,  78,   78,   78,   16,   78,   25,   78,   25,   25,   40,   497,  169,  79,
    565,  79,   497,  79,   78,   34,   1093, 168,  497,  79,   529,  530,  531,  79,   79,   23,
    26,   501,  189,  43,   78,   497,  541,  184,  79,   186,  497,  78,   547,  548,  549,  202,
    203,  204,  205,  587,  588,  78,   36,   558,  559,  79,   561,  595,  596,  597,  598,  599,
    600,  601,  602,  603,  604,  618,  619,  607,  575,  78,   25,   624,  625,  25,   618,  619,
    618,  619,  78,   78,   618,  619,  624,  625,  78,   78,   624,  625,  78,   618,  619,  79,
    35,   618,  619,  624,  625,  637,  78,   624,  625,  608,  609,  32,   28,   79,   79,   79,
    648,  79,   650,  651,  25,   79,   618,  619,  78,   78,   618,  619,  624,  625,  79,   79,
    624,  625,  79,   38,   79,   617,  79,   79,   79,   79,   617,  79,   620,  79,   610,  610,
    617,  79,   618,  619,  684,  614,  615,  79,   624,  625,  44,   79,   610,  617,  79,   78,
    696,  79,   617,  699,  700,  78,   610,  78,   618,  619,  78,   33,   618,  619,  624,  625,
    79,   713,  624,  625,  618,  619,  79,   79,   618,  619,  624,  625,  618,  619,  624,  625,
    618,  619,  624,  625,  79,   733,  624,  625,  79,   737,  78,   739,  79,   741,  1003, 1004,
    618,  619,  79,   29,   79,   749,  624,  625,  79,   39,   79,   755,  756,  79,   758,  79,
    78,   761,  79,   79,   764,  79,   766,  79,   79,   769,  770,  771,  772,  79,   79,   79,
    79,   79,   69,   70,   71,   39,   79,   74,   75,   76,   786,  70,   41,   79,   79,   79,
    79,   31,   79,   795,  45,   496,  79,   82,   79,   79,   79,   79,   79,   807,  79,   98,
    99,   100,  79,   79,   79,   26,   79,   79,   79,   806,  109,  110,  111,  501,  79,   79,
    79,   79,   1006, 79,   79,   79,   1071, 79,   79,   79,   79,   254,  625,  583,  1158, 1021,
    1264, 1277, 842,  1252, 1272, 1031, 991,  1070, 620,  -1,   -1,   -1,   96,   -1,   -1,   855,
    -1,   -1,   858,  -1,   860,  861,  862,  76,   807,  865,  -1,   815,  868,  -1,   807,  871,
    815,  873,  876,  -1,   -1,   -1,   815,  -1,   -1,   -1,   -1,   807,  884,  -1,   886,  -1,
    807,  889,  890,  815,  -1,   -1,   894,  832,  815,  897,  -1,   -1,   900,  -1,   -1,   903,
    -1,   -1,   -1,   907,  -1,   -1,   -1,   202,  203,  204,  205,  -1,   -1,   -1,   -1,   -1,
    920,  -1,   -1,   923,  924,  -1,   -1,   927,  928,  -1,   -1,   -1,   932,  -1,   -1,   935,
    -1,   -1,   938,  939,  940,  -1,   -1,   -1,   944,  -1,   946,  947,  515,  949,  -1,   518,
    952,  -1,   -1,   955,  -1,   957,  -1,   -1,   960,  961,  962,  963,  -1,   -1,   -1,   534,
    -1,   -1,   -1,   -1,   539,  540,  -1,   542,  543,  977,  -1,   -1,   -1,   981,  -1,   -1,
    984,  987,  -1,   554,  555,  -1,   1003, 1004, -1,   -1,   -1,   -1,   563,  -1,   -1,   -1,
    -1,   1003, 1004, -1,   -1,   1003, 1004, -1,   -1,   576,  -1,   -1,   579,  -1,   1003, 1004,
    583,  1017, 1003, 1004, -1,   -1,   -1,   -1,   -1,   -1,   -1,   1027, -1,   1029, 1030, 1031,
    -1,   -1,   -1,   -1,   -1,   -1,   1038, 1003, 1004, -1,   990,  1003, 1004, -1,   -1,   990,
    -1,   -1,   1050, -1,   1000, 990,  -1,   1055, -1,   1000, 1058, 1059, 1060, -1,   1062, 1000,
    -1,   1067, 990,  1003, 1004, 1071, -1,   990,  -1,   -1,   -1,   -1,   1000, -1,   -1,   -1,
    -1,   1000, -1,   -1,   1084, -1,   -1,   1087, 1088, 1003, 1004, 1091, -1,   1003, 1004, -1,
    1096, -1,   -1,   -1,   -1,   1003, 1004, 1103, 1104, 1003, 1004, -1,   -1,   1003, 1004, -1,
    -1,   1003, 1004, -1,   -1,   -1,   -1,   -1,   1120, 1069, 1122, 1123, -1,   -1,   1069, -1,
    1071, 1003, 1004, -1,   1069, -1,   1071, -1,   -1,   -1,   -1,   -1,   530,  531,  -1,   -1,
    -1,   1069, -1,   1071, -1,   1149, 1069, 541,  1071, 1153, -1,   1155, -1,   547,  548,  549,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   1167, 558,  559,  -1,   561,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   575,  1186, -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   1209, -1,   1211, -1,   -1,   1214, -1,   -1,   500,  608,  609,  503,  504,  505,  -1,
    -1,   -1,   -1,   -1,   -1,   512,  513,  514,  -1,   516,  517,  1235, 1236, -1,   -1,   -1,
    523,  524,  -1,   -1,   -1,   -1,   529,  530,  531,  532,  1250, -1,   535,  -1,   -1,   538,
    -1,   -1,   541,  -1,   -1,   -1,   -1,   546,  547,  548,  549,  -1,   551,  -1,   553,  1271,
    -1,   -1,   -1,   558,  559,  -1,   561,  -1,   -1,   -1,   -1,   -1,   567,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   575,  -1,   -1,   -1,   -1,   -1,   581,  582,  -1,   -1,   585,  -1,
    -1,   -1,   -1,   -1,   -1,   522,  593,  594,  525,  526,  527,  528,  529,  530,  531,  -1,
    -1,   -1,   -1,   536,  -1,   608,  609,  -1,   541,  -1,   -1,   -1,   -1,   -1,   547,  548,
    549,  -1,   -1,   -1,   -1,   -1,   -1,   556,  -1,   558,  559,  -1,   561,  -1,   633,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   571,  572,  -1,   574,  575,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   663,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   605,  -1,   -1,   608,  609,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   711,  -1,   3,    4,
    5,    6,    7,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,   20,
    -1,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
    37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   -1,   -1,
    53,   54,   -1,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,
    -1,   -1,   -1,   72,   73,   -1,   -1,   -1,   77,   -1,   79,   80,   -1,   82,   -1,   84,
    85,   86,   -1,   -1,   89,   90,   91,   92,   93,   804,  95,   96,   97,   -1,   -1,   -1,
    101,  102,  103,  104,  105,  106,  107,  108,  -1,   -1,   -1,   112,  113,  114,  115,  116,
    117,  118,  119,  120,  121,  -1,   123,  124,  125,  126,  -1,   -1,   129,  130,  131,  132,
    133,  134,  135,  -1,   -1,   138,  139,  140,  141,  142,  143,  144,  145,  146,  -1,   148,
    149,  150,  151,  152,  153,  154,  155,  156,  -1,   -1,   159,  160,  161,  162,  163,  164,
    165,  166,  167,  -1,   -1,   170,  171,  172,  173,  174,  175,  176,  177,  178,  179,  180,
    181,  182,  183,  -1,   185,  186,  187,  3,    4,    5,    6,    7,    8,    9,    10,   11,
    12,   13,   14,   15,   16,   17,   18,   19,   20,   -1,   22,   23,   24,   25,   26,   27,
    28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,
    44,   45,   46,   47,   48,   49,   50,   -1,   -1,   53,   54,   -1,   56,   57,   58,   59,
    60,   61,   62,   63,   64,   65,   66,   67,   68,   -1,   -1,   -1,   72,   73,   -1,   -1,
    -1,   77,   -1,   -1,   80,   -1,   82,   -1,   84,   85,   86,   -1,   -1,   89,   90,   91,
    92,   93,   -1,   95,   96,   97,   -1,   -1,   -1,   101,  102,  103,  104,  105,  106,  107,
    108,  -1,   -1,   -1,   112,  113,  114,  115,  116,  117,  118,  119,  120,  121,  -1,   123,
    124,  125,  126,  -1,   -1,   129,  130,  131,  132,  133,  134,  135,  -1,   -1,   138,  139,
    140,  141,  142,  143,  144,  145,  146,  -1,   148,  149,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   159,  160,  161,  162,  163,  164,  165,  166,  167,  -1,   -1,   170,  171,
    172,  173,  174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  -1,   185,  186,  187,
    3,    4,    5,    6,    7,    8,    9,    10,   11,   12,   13,   14,   15,   16,   17,   18,
    19,   20,   -1,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,
    35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,
    -1,   -1,   53,   54,   -1,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,   66,
    67,   68,   -1,   -1,   -1,   72,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   -1,   82,
    -1,   84,   85,   86,   -1,   -1,   89,   90,   91,   92,   93,   -1,   95,   96,   97,   -1,
    -1,   -1,   101,  102,  103,  104,  105,  106,  107,  108,  -1,   -1,   -1,   112,  113,  114,
    115,  116,  117,  118,  119,  120,  121,  -1,   123,  124,  125,  126,  -1,   -1,   129,  130,
    131,  132,  133,  134,  135,  -1,   -1,   138,  139,  140,  141,  142,  143,  144,  145,  146,
    -1,   148,  149,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  160,  161,  162,
    163,  164,  165,  166,  167,  -1,   -1,   170,  171,  172,  173,  174,  175,  176,  177,  178,
    179,  180,  181,  182,  183,  -1,   185,  186,  187,  3,    4,    5,    6,    7,    8,    9,
    10,   11,   12,   13,   14,   15,   16,   17,   18,   19,   20,   -1,   22,   23,   24,   25,
    26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,
    42,   43,   44,   45,   46,   47,   48,   49,   50,   -1,   -1,   53,   54,   -1,   56,   57,
    58,   59,   60,   61,   62,   63,   64,   65,   66,   67,   68,   -1,   -1,   -1,   72,   73,
    -1,   -1,   -1,   77,   -1,   79,   80,   -1,   82,   -1,   84,   85,   86,   -1,   -1,   89,
    90,   91,   92,   93,   -1,   95,   96,   97,   -1,   -1,   -1,   101,  102,  103,  104,  105,
    106,  107,  108,  -1,   -1,   -1,   112,  113,  114,  115,  116,  117,  118,  119,  120,  121,
    -1,   123,  124,  125,  126,  -1,   -1,   129,  130,  131,  132,  133,  134,  135,  -1,   -1,
    138,  139,  140,  141,  142,  143,  144,  145,  146,  -1,   148,  149,  150,  151,  152,  153,
    154,  155,  156,  -1,   -1,   159,  160,  161,  162,  163,  164,  165,  166,  167,  -1,   -1,
    170,  171,  172,  173,  174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  -1,   185,
    186,  3,    4,    5,    6,    7,    8,    9,    -1,   11,   12,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   46,   47,   48,   49,
    50,   -1,   -1,   53,   54,   -1,   56,   57,   58,   59,   60,   61,   62,   63,   64,   65,
    66,   67,   68,   -1,   -1,   -1,   72,   73,   -1,   -1,   -1,   -1,   -1,   -1,   80,   -1,
    82,   -1,   84,   85,   86,   -1,   -1,   89,   90,   91,   -1,   93,   -1,   95,   96,   97,
    -1,   -1,   -1,   101,  102,  103,  104,  105,  106,  107,  108,  -1,   -1,   -1,   112,  113,
    114,  115,  116,  117,  118,  119,  120,  121,  -1,   123,  124,  125,  126,  -1,   -1,   129,
    130,  131,  132,  133,  134,  135,  -1,   -1,   138,  139,  140,  141,  142,  143,  144,  145,
    146,  -1,   148,  149,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   159,  160,  161,
    162,  163,  164,  165,  166,  167,  -1,   -1,   170,  171,  172,  173,  174,  175,  176,  177,
    178,  179,  180,  181,  182,  183,  10,   185,  -1,   13,   14,   15,   16,   17,   18,   19,
    20,   -1,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,
    36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   79,   -1,   10,   -1,   -1,
    13,   14,   15,   16,   17,   18,   19,   20,   92,   22,   23,   24,   25,   26,   27,   28,
    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,
    45,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   79,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   92,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   10,   186,  187,  13,   14,   15,   16,   17,   18,   19,   20,
    -1,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
    37,   38,   39,   40,   41,   42,   43,   44,   45,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   79,   -1,   10,   186,  187,  13,
    14,   15,   16,   17,   18,   19,   20,   92,   22,   23,   24,   25,   26,   27,   28,   29,
    30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   43,   44,   45,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   79,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   92,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   10,   186,  187,  13,   14,   15,   16,   17,   18,   19,   20,   -1,
    22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   37,
    38,   39,   40,   41,   42,   43,   44,   45,   -1,   -1,   -1,   -1,   51,   52,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,
    71,   -1,   -1,   74,   75,   76,   -1,   78,   -1,   79,   -1,   -1,   186,  187,  -1,   -1,
    87,   88,   -1,   -1,   -1,   -1,   92,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   186,  -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,
    199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,
    -1,   74,   75,   76,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,
    -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,
    137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,
    201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,
    75,   76,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,
    -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,
    203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,
    -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,
    -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,
    205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   78,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,
    -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,
    111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,
    191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,
    207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   78,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,
    -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,
    193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,
    51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   78,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,
    99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,
    197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,
    71,   -1,   -1,   74,   75,   76,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,
    199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,
    -1,   74,   75,   76,   -1,   78,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,
    -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,
    137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,
    201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,
    75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,
    -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,
    203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,
    -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    109,  110,  111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    189,  190,  191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,
    205,  206,  207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,
    -1,   -1,   -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,
    111,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,
    191,  192,  193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,
    207,  208,  51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,
    -1,   98,   99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,
    193,  194,  195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,
    51,   52,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,
    99,   100,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    147,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   158,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  190,  191,  192,  193,  194,
    195,  196,  197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    69,   70,   71,   -1,   -1,   74,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,   136,  137,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   147,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   169,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   189,  -1,   -1,   192,  193,  194,  195,  196,
    197,  198,  199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  51,   52,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   69,   70,
    71,   -1,   -1,   74,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    87,   88,   -1,   -1,   -1,   -1,   -1,   94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   109,  110,  111,  -1,   69,   70,   71,   -1,   -1,   74,
    75,   76,   -1,   78,   -1,   -1,   -1,   -1,   127,  128,  -1,   -1,   87,   88,   -1,   -1,
    -1,   136,  137,  94,   -1,   -1,   -1,   98,   99,   100,  -1,   -1,   147,  -1,   -1,   -1,
    -1,   -1,   109,  110,  111,  -1,   157,  158,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   169,  -1,   127,  128,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   136,  137,  -1,
    -1,   -1,   -1,   -1,   -1,   -1,   189,  -1,   147,  192,  193,  194,  195,  196,  197,  198,
    199,  200,  201,  202,  203,  204,  205,  206,  207,  208,  503,  504,  505,  -1,   169,  -1,
    -1,   -1,   -1,   512,  513,  514,  -1,   516,  517,  -1,   -1,   -1,   -1,   -1,   523,  524,
    -1,   -1,   189,  -1,   -1,   -1,   -1,   532,  -1,   -1,   535,  -1,   -1,   538,  -1,   202,
    203,  204,  205,  -1,   -1,   546,  -1,   -1,   504,  505,  551,  -1,   553,  -1,   -1,   -1,
    512,  513,  514,  -1,   516,  517,  -1,   -1,   -1,   -1,   567,  523,  524,  -1,   -1,   -1,
    -1,   -1,   -1,   -1,   532,  -1,   -1,   535,  581,  582,  538,  -1,   585,  -1,   -1,   -1,
    -1,   -1,   546,  -1,   593,  594,  -1,   551,  -1,   553,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   -1,   -1,   567,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,   -1,   -1,   581,  582,  -1,   -1,   585,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   593,  594};

const short ParserGen::yystos_[] = {
    0,   209, 210, 211, 212, 468, 157, 262, 158, 429, 158, 452, 158, 462, 0,   158, 263, 430, 453,
    463, 150, 151, 152, 153, 154, 155, 156, 264, 265, 266, 267, 268, 269, 270, 271, 78,  8,   10,
    13,  14,  15,  16,  17,  18,  19,  20,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
    33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  55,  79,  83,  92,  122, 124,
    168, 184, 186, 218, 221, 223, 227, 232, 434, 435, 436, 442, 443, 445, 446, 447, 79,  92,  186,
    187, 215, 219, 232, 460, 3,   4,   5,   6,   7,   8,   9,   11,  12,  46,  47,  48,  49,  50,
    53,  54,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  72,  73,  77,  79,
    80,  82,  84,  85,  86,  89,  90,  91,  93,  95,  96,  97,  101, 102, 103, 104, 105, 106, 107,
    108, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 123, 124, 125, 126, 129, 130, 131, 132,
    133, 134, 135, 138, 139, 140, 141, 142, 143, 144, 145, 146, 148, 149, 150, 151, 152, 153, 154,
    155, 156, 159, 160, 161, 162, 163, 164, 165, 166, 167, 170, 171, 172, 173, 174, 175, 176, 177,
    178, 179, 180, 181, 182, 183, 185, 187, 217, 218, 220, 221, 222, 223, 224, 226, 467, 158, 69,
    70,  71,  74,  75,  76,  98,  99,  100, 109, 110, 111, 202, 203, 204, 205, 237, 239, 240, 241,
    278, 429, 158, 158, 278, 158, 469, 79,  51,  52,  87,  88,  94,  127, 128, 136, 137, 147, 157,
    158, 169, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 206, 207, 208, 233,
    234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252,
    253, 254, 255, 256, 257, 259, 157, 158, 254, 279, 282, 283, 284, 286, 287, 288, 289, 290, 291,
    292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 307, 308, 309, 310, 311,
    312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330,
    331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368,
    369, 370, 371, 372, 373, 374, 375, 376, 377, 378, 398, 399, 400, 401, 402, 403, 404, 405, 406,
    407, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420, 421, 424, 425, 469, 234,
    245, 158, 254, 432, 157, 51,  52,  69,  70,  71,  74,  75,  76,  98,  99,  100, 109, 110, 111,
    158, 202, 203, 204, 205, 233, 234, 235, 236, 238, 242, 243, 245, 247, 248, 249, 251, 252, 253,
    276, 283, 425, 454, 455, 456, 457, 458, 454, 69,  70,  74,  75,  98,  99,  109, 110, 158, 464,
    465, 79,  272, 42,  15,  263, 396, 258, 395, 3,   4,   5,   6,   7,   8,   9,   11,  12,  46,
    47,  48,  49,  50,  53,  54,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,
    72,  73,  80,  82,  84,  85,  86,  89,  90,  91,  93,  95,  96,  97,  101, 102, 103, 104, 105,
    106, 107, 108, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 123, 124, 125, 126, 129, 130,
    131, 132, 133, 134, 135, 138, 139, 140, 141, 142, 143, 144, 145, 146, 148, 149, 159, 160, 161,
    162, 163, 164, 165, 166, 167, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182,
    183, 185, 285, 10,  448, 431, 433, 77,  92,  146, 215, 225, 459, 461, 115, 79,  92,  214, 215,
    228, 278, 245, 78,  244, 79,  226, 231, 78,  279, 157, 254, 284, 422, 423, 424, 426, 427, 427,
    427, 283, 157, 283, 157, 157, 280, 279, 427, 427, 427, 280, 427, 427, 280, 157, 157, 157, 469,
    427, 427, 469, 469, 469, 469, 158, 254, 424, 426, 428, 469, 426, 428, 469, 426, 428, 469, 427,
    157, 280, 427, 469, 279, 427, 280, 280, 426, 428, 469, 280, 157, 280, 281, 157, 157, 427, 426,
    428, 469, 426, 428, 469, 426, 428, 469, 157, 427, 157, 427, 280, 280, 469, 87,  88,  94,  127,
    128, 136, 137, 147, 169, 426, 428, 469, 426, 428, 469, 157, 426, 428, 469, 157, 280, 157, 283,
    157, 427, 306, 469, 306, 306, 469, 469, 157, 469, 426, 428, 469, 280, 157, 157, 280, 157, 427,
    427, 157, 280, 281, 157, 427, 157, 279, 279, 157, 157, 157, 157, 427, 427, 279, 279, 279, 279,
    279, 279, 279, 279, 279, 279, 469, 157, 279, 426, 428, 469, 426, 428, 469, 79,  216, 218, 221,
    223, 230, 250, 20,  449, 79,  81,  118, 123, 182, 437, 438, 439, 440, 441, 429, 429, 157, 278,
    455, 455, 79,  461, 127, 169, 466, 158, 274, 275, 276, 277, 424, 274, 79,  37,  244, 279, 79,
    79,  79,  79,  279, 79,  279, 279, 79,  79,  79,  79,  79,  79,  79,  79,  79,  395, 395, 244,
    25,  79,  79,  19,  390, 18,  17,  17,  79,  79,  17,  79,  79,  17,  79,  79,  17,  79,  279,
    79,  79,  13,  381, 79,  79,  79,  79,  79,  79,  17,  79,  279, 79,  79,  279, 279, 79,  79,
    79,  17,  79,  79,  17,  79,  79,  17,  244, 79,  279, 79,  79,  79,  14,  384, 79,  79,  79,
    79,  79,  79,  79,  79,  79,  79,  79,  17,  79,  79,  17,  279, 79,  79,  17,  279, 79,  279,
    79,  279, 79,  79,  25,  79,  79,  22,  22,  279, 384, 79,  79,  17,  79,  279, 279, 79,  279,
    79,  79,  279, 79,  79,  279, 79,  279, 79,  79,  279, 279, 279, 279, 79,  79,  79,  79,  79,
    79,  79,  79,  79,  79,  79,  79,  384, 279, 79,  79,  79,  17,  79,  79,  17,  279, 250, 30,
    450, 244, 157, 158, 253, 157, 245, 260, 261, 278, 78,  79,  278, 79,  79,  214, 225, 229, 273,
    240, 78,  78,  78,  279, 78,  78,  78,  279, 279, 24,  386, 279, 279, 279, 279, 279, 279, 279,
    245, 16,  279, 279, 279, 279, 279, 279, 279, 78,  279, 279, 25,  279, 279, 279, 279, 279, 78,
    279, 279, 279, 279, 279, 25,  279, 279, 279, 279, 279, 279, 279, 279, 279, 279, 279, 25,  279,
    279, 279, 245, 40,  451, 278, 431, 444, 278, 275, 275, 79,  229, 79,  79,  79,  78,  79,  79,
    79,  34,  379, 279, 27,  31,  389, 394, 23,  382, 26,  392, 382, 43,  383, 383, 383, 78,  279,
    383, 279, 279, 397, 397, 383, 383, 383, 79,  78,  279, 383, 383, 78,  383, 395, 79,  78,  36,
    385, 25,  25,  78,  279, 383, 395, 395, 395, 279, 78,  78,  279, 279, 279, 78,  279, 78,  383,
    383, 245, 79,  278, 437, 78,  261, 78,  279, 35,  380, 279, 279, 32,  387, 28,  391, 279, 383,
    250, 383, 383, 279, 79,  79,  79,  79,  25,  79,  78,  279, 279, 78,  78,  79,  79,  79,  79,
    79,  79,  79,  79,  79,  78,  79,  279, 38,  279, 279, 79,  79,  79,  78,  78,  78,  79,  79,
    78,  78,  78,  79,  79,  79,  79,  79,  78,  79,  79,  279, 44,  279, 33,  393, 279, 29,  379,
    79,  380, 79,  79,  79,  279, 79,  78,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  279,
    39,  39,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  79,  279, 279, 41,  388, 279, 380,
    79,  79,  79,  79,  79,  279, 279, 79,  279, 383, 389, 79,  79,  79,  79,  79,  79,  45,  387,
    79,  79,  79,  279, 393, 79,  388, 79,  383, 79,  79};

const short ParserGen::yyr1_[] = {
    0,   213, 468, 468, 468, 468, 262, 263, 263, 469, 264, 264, 264, 264, 264, 264, 264, 271, 265,
    266, 278, 278, 278, 278, 267, 268, 269, 270, 272, 272, 228, 228, 274, 275, 275, 275, 276, 276,
    276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276,
    276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 276, 214, 215, 215, 215, 277, 273, 273,
    229, 229, 429, 430, 430, 434, 434, 434, 434, 434, 434, 435, 432, 432, 431, 431, 437, 437, 437,
    437, 440, 260, 444, 444, 261, 261, 441, 441, 442, 438, 438, 439, 436, 443, 443, 443, 433, 433,
    227, 227, 227, 221, 445, 446, 448, 448, 449, 449, 450, 450, 451, 447, 447, 217, 217, 217, 217,
    217, 217, 217, 218, 219, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232,
    232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232, 232,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 245, 245, 245, 245, 245, 245, 245,
    245, 245, 245, 246, 259, 247, 248, 249, 251, 252, 253, 233, 234, 235, 236, 238, 242, 243, 237,
    237, 237, 237, 239, 239, 239, 239, 240, 240, 240, 240, 241, 241, 241, 241, 250, 250, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
    395, 395, 279, 279, 279, 279, 422, 422, 428, 428, 423, 423, 424, 424, 425, 425, 425, 425, 425,
    425, 425, 425, 425, 425, 425, 280, 281, 282, 282, 283, 426, 427, 427, 284, 285, 285, 230, 216,
    216, 216, 223, 224, 225, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286, 286,
    286, 286, 287, 287, 287, 287, 287, 287, 287, 287, 287, 406, 406, 406, 406, 406, 406, 406, 406,
    406, 406, 406, 406, 406, 406, 406, 288, 419, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364,
    365, 366, 367, 368, 369, 407, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 420, 421,
    289, 289, 289, 290, 291, 292, 370, 370, 370, 370, 370, 370, 370, 370, 371, 372, 373, 381, 381,
    374, 375, 376, 377, 377, 377, 378, 296, 296, 296, 296, 296, 296, 296, 296, 296, 296, 296, 296,
    296, 296, 296, 296, 296, 296, 296, 296, 296, 296, 297, 382, 382, 383, 383, 298, 299, 328, 328,
    328, 328, 328, 328, 328, 328, 328, 328, 328, 328, 328, 328, 328, 386, 386, 387, 387, 388, 388,
    389, 389, 390, 390, 394, 394, 391, 391, 392, 392, 393, 393, 329, 329, 330, 331, 331, 331, 332,
    332, 332, 335, 335, 335, 333, 333, 333, 334, 334, 334, 340, 340, 340, 342, 342, 342, 336, 336,
    336, 337, 337, 337, 343, 343, 343, 341, 341, 341, 338, 338, 338, 339, 339, 339, 397, 397, 397,
    300, 301, 384, 384, 302, 309, 319, 385, 385, 306, 303, 304, 305, 307, 308, 310, 311, 312, 313,
    314, 315, 316, 317, 318, 466, 466, 464, 462, 463, 463, 465, 465, 465, 465, 465, 465, 465, 465,
    222, 222, 467, 467, 452, 453, 453, 460, 460, 454, 455, 455, 455, 455, 455, 457, 456, 456, 458,
    459, 459, 461, 461, 398, 398, 398, 398, 398, 398, 398, 399, 400, 401, 402, 403, 404, 405, 293,
    293, 294, 295, 244, 244, 255, 255, 256, 396, 396, 257, 258, 258, 231, 226, 226, 226, 226, 226,
    226, 320, 320, 320, 320, 320, 320, 320, 321, 322, 323, 324, 325, 326, 327, 344, 344, 344, 344,
    344, 344, 344, 344, 344, 344, 379, 379, 380, 380, 345, 346, 347, 348, 349, 350, 351, 352, 353,
    354};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 2, 3, 0, 4, 1, 1, 1,  1, 1, 1, 1, 1,  5,  3, 7, 1, 1, 1,  1,  2, 2, 2, 4, 0,
    2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 3, 1,  2,  2, 2, 3, 0, 2,  1,  1, 1, 1, 1, 1,
    2, 1, 3, 0, 2, 1, 1, 1, 1, 2, 3, 0,  2, 1, 1, 2, 2,  2,  2, 5, 5, 5, 1,  1,  1, 0, 2, 1, 1,
    1, 1, 2, 7, 0, 2, 0, 2, 0, 2, 2, 2,  2, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  0,  2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 4, 5, 4, 4,  3,  3, 1, 1, 3, 0,
    2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  4, 4, 4, 4, 4,
    4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1,  1, 1, 1, 1, 1,  1,  1, 4, 4, 4, 4,  7,  4, 4, 4, 7, 4,
    7, 8, 7, 7, 4, 7, 7, 4, 4, 4, 4, 4,  4, 4, 4, 4, 4,  4,  4, 4, 4, 1, 1,  1,  4, 4, 6, 1, 1,
    1, 1, 1, 1, 1, 1, 4, 4, 6, 0, 2, 10, 4, 4, 9, 4, 4,  4,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 6,  0, 2, 0, 2, 11, 10, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 0, 2, 0, 2, 0, 2, 0, 2,  0, 2, 0, 2, 0,  2,  0, 2, 0, 2, 14, 16, 9, 4, 8, 4, 4,
    8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4,  8, 4, 4, 8, 4,  4,  8, 4, 4, 8, 4,  4,  8, 4, 4, 8, 4,
    4, 8, 4, 4, 8, 4, 0, 1, 2, 8, 8, 0,  2, 8, 8, 8, 0,  2,  7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8,
    8, 8, 4, 4, 1, 1, 4, 3, 0, 2, 1, 1,  1, 1, 1, 1, 1,  1,  1, 1, 2, 2, 3,  0,  2, 2, 2, 1, 1,
    1, 1, 1, 1, 4, 4, 7, 3, 1, 2, 2, 2,  1, 1, 1, 1, 1,  1,  1, 6, 6, 4, 8,  8,  4, 8, 1, 1, 6,
    6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2,  1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,  1,  1, 4, 4, 4, 4,
    4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4,  4,  4, 4, 4};


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
                                           "\"$caseSensitive argument\"",
                                           "ARRAY_ELEM_AT",
                                           "ARRAY_TO_OBJECT",
                                           "\"as argument\"",
                                           "\"chars argument\"",
                                           "\"coll argument\"",
                                           "\"cond argument\"",
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
                                           "CONCAT_ARRAYS",
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
                                           "FILTER",
                                           "FIRST",
                                           "FLOOR",
                                           "\"geoNearDistance\"",
                                           "\"geoNearPoint\"",
                                           "GT",
                                           "GTE",
                                           "HOUR",
                                           "ID",
                                           "IN",
                                           "\"indexKey\"",
                                           "INDEX_OF_ARRAY",
                                           "INDEX_OF_BYTES",
                                           "INDEX_OF_CP",
                                           "\"-1 (int)\"",
                                           "\"1 (int)\"",
                                           "\"zero (int)\"",
                                           "IS_ARRAY",
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
                                           "arrayExps",
                                           "arrayElemAt",
                                           "arrayToObject",
                                           "concatArrays",
                                           "filter",
                                           "first",
                                           "in",
                                           "indexOfArray",
                                           "isArray",
                                           "onErrorArg",
                                           "onNullArg",
                                           "asArg",
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
#endif


#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    417,  417,  420,  423,  426,  433,  439,  440,  448,  451,  451,  451,  451,  451,  451,
    451,  454,  464,  470,  480,  480,  480,  480,  484,  489,  494,  500,  519,  522,  529,  532,
    538,  552,  553,  554,  558,  559,  560,  561,  562,  563,  564,  565,  566,  567,  568,  569,
    572,  575,  578,  581,  584,  587,  590,  593,  596,  599,  602,  605,  608,  611,  614,  617,
    620,  623,  624,  625,  626,  631,  640,  651,  652,  667,  674,  678,  686,  689,  695,  701,
    704,  711,  712,  715,  716,  717,  718,  721,  730,  731,  737,  740,  748,  748,  748,  748,
    752,  758,  764,  765,  772,  772,  776,  785,  795,  801,  806,  815,  825,  833,  834,  835,
    838,  841,  848,  848,  848,  851,  857,  863,  882,  885,  890,  893,  898,  901,  906,  912,
    913,  919,  922,  925,  928,  931,  934,  937,  943,  949,  965,  968,  971,  974,  977,  980,
    983,  986,  989,  992,  995,  998,  1001, 1004, 1007, 1010, 1013, 1016, 1019, 1022, 1025, 1028,
    1031, 1034, 1037, 1040, 1043, 1046, 1049, 1052, 1055, 1058, 1061, 1069, 1072, 1075, 1078, 1081,
    1084, 1087, 1090, 1093, 1096, 1099, 1102, 1105, 1108, 1111, 1114, 1117, 1120, 1123, 1126, 1129,
    1132, 1135, 1138, 1141, 1144, 1147, 1150, 1153, 1156, 1159, 1162, 1165, 1168, 1171, 1174, 1177,
    1180, 1183, 1186, 1189, 1192, 1195, 1198, 1201, 1204, 1207, 1210, 1213, 1216, 1219, 1222, 1225,
    1228, 1231, 1234, 1237, 1240, 1243, 1246, 1249, 1252, 1255, 1258, 1261, 1264, 1267, 1270, 1273,
    1276, 1279, 1282, 1285, 1288, 1291, 1294, 1297, 1300, 1303, 1306, 1309, 1312, 1315, 1318, 1321,
    1324, 1327, 1330, 1333, 1336, 1339, 1342, 1345, 1348, 1351, 1354, 1357, 1360, 1363, 1366, 1369,
    1372, 1375, 1378, 1381, 1384, 1387, 1394, 1399, 1402, 1405, 1408, 1411, 1414, 1417, 1420, 1423,
    1429, 1443, 1457, 1463, 1469, 1475, 1481, 1487, 1493, 1499, 1505, 1511, 1517, 1523, 1529, 1535,
    1538, 1541, 1544, 1550, 1553, 1556, 1559, 1565, 1568, 1571, 1574, 1580, 1583, 1586, 1589, 1595,
    1598, 1604, 1605, 1606, 1607, 1608, 1609, 1610, 1611, 1612, 1613, 1614, 1615, 1616, 1617, 1618,
    1619, 1620, 1621, 1622, 1623, 1624, 1631, 1632, 1639, 1639, 1639, 1639, 1643, 1643, 1647, 1647,
    1651, 1651, 1655, 1655, 1659, 1659, 1659, 1659, 1659, 1659, 1659, 1660, 1660, 1660, 1660, 1665,
    1672, 1678, 1682, 1691, 1698, 1703, 1703, 1708, 1714, 1717, 1724, 1731, 1731, 1731, 1735, 1741,
    1747, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1753, 1754, 1754,
    1754, 1758, 1761, 1764, 1767, 1770, 1773, 1776, 1779, 1782, 1787, 1787, 1787, 1787, 1787, 1787,
    1787, 1787, 1787, 1787, 1787, 1787, 1787, 1788, 1788, 1792, 1799, 1805, 1810, 1815, 1821, 1826,
    1831, 1836, 1842, 1847, 1853, 1862, 1868, 1874, 1879, 1885, 1891, 1896, 1901, 1906, 1911, 1916,
    1921, 1926, 1931, 1936, 1941, 1946, 1951, 1956, 1962, 1962, 1962, 1966, 1973, 1980, 1987, 1987,
    1987, 1987, 1987, 1987, 1987, 1987, 1991, 1998, 2004, 2011, 2014, 2020, 2027, 2033, 2040, 2045,
    2049, 2056, 2062, 2062, 2062, 2062, 2062, 2062, 2062, 2063, 2063, 2063, 2063, 2063, 2063, 2063,
    2063, 2064, 2064, 2064, 2064, 2064, 2064, 2064, 2068, 2075, 2078, 2084, 2087, 2094, 2103, 2112,
    2112, 2112, 2112, 2112, 2112, 2112, 2112, 2112, 2113, 2113, 2113, 2113, 2113, 2113, 2117, 2120,
    2126, 2129, 2135, 2138, 2144, 2147, 2153, 2156, 2162, 2165, 2171, 2174, 2180, 2183, 2189, 2192,
    2198, 2204, 2213, 2221, 2224, 2228, 2234, 2238, 2242, 2248, 2252, 2256, 2262, 2266, 2270, 2276,
    2280, 2284, 2290, 2294, 2298, 2304, 2308, 2312, 2318, 2322, 2326, 2332, 2336, 2340, 2346, 2350,
    2354, 2360, 2364, 2368, 2374, 2378, 2382, 2388, 2392, 2396, 2402, 2405, 2408, 2414, 2425, 2436,
    2439, 2445, 2453, 2461, 2469, 2472, 2477, 2486, 2492, 2498, 2504, 2514, 2524, 2531, 2538, 2545,
    2553, 2561, 2569, 2577, 2583, 2589, 2592, 2598, 2604, 2609, 2612, 2619, 2622, 2625, 2628, 2631,
    2634, 2637, 2640, 2645, 2647, 2657, 2659, 2665, 2684, 2687, 2694, 2697, 2703, 2717, 2718, 2719,
    2720, 2721, 2725, 2731, 2734, 2742, 2749, 2753, 2761, 2764, 2770, 2770, 2770, 2770, 2770, 2770,
    2771, 2775, 2781, 2787, 2794, 2805, 2816, 2823, 2834, 2834, 2838, 2845, 2852, 2852, 2856, 2856,
    2860, 2866, 2867, 2874, 2880, 2883, 2890, 2897, 2898, 2899, 2900, 2901, 2902, 2905, 2905, 2905,
    2905, 2905, 2905, 2905, 2907, 2912, 2917, 2922, 2927, 2932, 2937, 2943, 2944, 2945, 2946, 2947,
    2948, 2949, 2950, 2951, 2952, 2957, 2960, 2967, 2970, 2976, 2986, 2991, 2996, 3001, 3006, 3011,
    3016, 3021, 3026};

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
#line 10172 "parser_gen.cpp"

#line 3030 "grammar.yy"
