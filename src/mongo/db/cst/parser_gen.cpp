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

#line 73 "src/mongo/db/cst/parser_gen.cpp"


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

#line 57 "src/mongo/db/cst/grammar.yy"
namespace mongo {
#line 166 "src/mongo/db/cst/parser_gen.cpp"

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
        case symbol_kind::S_match:                              // match
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
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
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
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
        case symbol_kind::S_match:                              // match
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
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
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
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
        case symbol_kind::S_match:                              // match
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
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
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
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
        case symbol_kind::S_match:                              // match
        case symbol_kind::S_predicates:                         // predicates
        case symbol_kind::S_compoundMatchExprs:                 // compoundMatchExprs
        case symbol_kind::S_predValue:                          // predValue
        case symbol_kind::S_additionalExprs:                    // additionalExprs
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
        case symbol_kind::S_logicalExpr:                       // logicalExpr
        case symbol_kind::S_operatorExpression:                // operatorExpression
        case symbol_kind::S_notExpr:                           // notExpr
        case symbol_kind::S_existsExpr:                        // existsExpr
        case symbol_kind::S_typeExpr:                          // typeExpr
        case symbol_kind::S_commentExpr:                       // commentExpr
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
                case symbol_kind::S_match:                       // match
                case symbol_kind::S_predicates:                  // predicates
                case symbol_kind::S_compoundMatchExprs:          // compoundMatchExprs
                case symbol_kind::S_predValue:                   // predValue
                case symbol_kind::S_additionalExprs:             // additionalExprs
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
                case symbol_kind::S_logicalExpr:                // logicalExpr
                case symbol_kind::S_operatorExpression:         // operatorExpression
                case symbol_kind::S_notExpr:                    // notExpr
                case symbol_kind::S_existsExpr:                 // existsExpr
                case symbol_kind::S_typeExpr:                   // typeExpr
                case symbol_kind::S_commentExpr:                // commentExpr
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
#line 393 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2193 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH match
#line 396 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2201 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 4:  // start: START_PROJECT findProject
#line 399 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2209 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 5:  // start: START_SORT sortSpecs
#line 402 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2217 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 6:  // pipeline: "array" stageList "end of array"
#line 409 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2225 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 7:  // stageList: %empty
#line 415 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 2231 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 8:  // stageList: "object" stage "end of object" stageList
#line 416 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2239 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 9:  // START_ORDERED_OBJECT: "object"
#line 424 "src/mongo/db/cst/grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2245 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2251 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2257 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2263 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2269 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 14:  // stage: project
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2275 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 15:  // stage: sample
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2281 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 16:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 430 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2293 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 17:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 440 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2301 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 18:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 446 "src/mongo/db/cst/grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2314 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 19:  // num: int
#line 456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2320 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 20:  // num: long
#line 456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2326 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 21:  // num: double
#line 456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2332 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 22:  // num: decimal
#line 456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2338 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 23:  // skip: STAGE_SKIP num
#line 460 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2346 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 24:  // limit: STAGE_LIMIT num
#line 465 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2354 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 25:  // project: STAGE_PROJECT "object" aggregationProjectFields "end of
                              // object"
#line 470 "src/mongo/db/cst/grammar.yy"
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
#line 2375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 26:  // aggregationProjectFields: %empty
#line 489 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2383 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 27:  // aggregationProjectFields: aggregationProjectFields
                              // aggregationProjectField
#line 492 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2392 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 28:  // aggregationProjectField: ID topLevelAggregationProjection
#line 499 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2400 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 29:  // aggregationProjectField: aggregationProjectionFieldname
                              // topLevelAggregationProjection
#line 502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2408 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 30:  // topLevelAggregationProjection: aggregationProjection
#line 508 "src/mongo/db/cst/grammar.yy"
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
#line 2424 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 31:  // aggregationProjection: projectionCommon
#line 522 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2430 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 32:  // aggregationProjection: aggregationProjectionObject
#line 523 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2436 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 33:  // aggregationProjection: aggregationOperator
#line 524 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2442 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 34:  // projectionCommon: string
#line 528 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2448 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 35:  // projectionCommon: binary
#line 529 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2454 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 36:  // projectionCommon: undefined
#line 530 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2460 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 37:  // projectionCommon: objectId
#line 531 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2466 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 38:  // projectionCommon: date
#line 532 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2472 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 39:  // projectionCommon: null
#line 533 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 40:  // projectionCommon: regex
#line 534 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2484 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 41:  // projectionCommon: dbPointer
#line 535 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2490 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 42:  // projectionCommon: javascript
#line 536 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2496 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 43:  // projectionCommon: symbol
#line 537 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 44:  // projectionCommon: javascriptWScope
#line 538 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2508 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 45:  // projectionCommon: "1 (int)"
#line 539 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2516 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 46:  // projectionCommon: "-1 (int)"
#line 542 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 47:  // projectionCommon: "arbitrary integer"
#line 545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2532 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 48:  // projectionCommon: "zero (int)"
#line 548 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2540 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 49:  // projectionCommon: "1 (long)"
#line 551 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2548 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 50:  // projectionCommon: "-1 (long)"
#line 554 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2556 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 51:  // projectionCommon: "arbitrary long"
#line 557 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2564 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 52:  // projectionCommon: "zero (long)"
#line 560 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2572 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 53:  // projectionCommon: "1 (double)"
#line 563 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2580 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 54:  // projectionCommon: "-1 (double)"
#line 566 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2588 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 55:  // projectionCommon: "arbitrary double"
#line 569 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2596 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 56:  // projectionCommon: "zero (double)"
#line 572 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2604 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 57:  // projectionCommon: "1 (decimal)"
#line 575 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2612 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 58:  // projectionCommon: "-1 (decimal)"
#line 578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2620 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 59:  // projectionCommon: "arbitrary decimal"
#line 581 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2628 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 60:  // projectionCommon: "zero (decimal)"
#line 584 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2636 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 61:  // projectionCommon: "true"
#line 587 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2644 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 62:  // projectionCommon: "false"
#line 590 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2652 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 63:  // projectionCommon: timestamp
#line 593 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2658 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 64:  // projectionCommon: minKey
#line 594 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2664 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 65:  // projectionCommon: maxKey
#line 595 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2670 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 66:  // projectionCommon: expressionArray
#line 596 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2676 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 67:  // aggregationProjectionFieldname: projectionFieldname
#line 601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2686 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 68:  // projectionFieldname: "fieldname"
#line 610 "src/mongo/db/cst/grammar.yy"
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
#line 2702 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 69:  // projectionFieldname: argAsProjectionPath
#line 621 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2708 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 70:  // projectionFieldname: "fieldname containing dotted path"
#line 622 "src/mongo/db/cst/grammar.yy"
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
#line 2724 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 71:  // aggregationProjectionObject: "object"
                              // aggregationProjectionObjectFields "end of object"
#line 637 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2732 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 72:  // aggregationProjectionObjectFields: aggregationProjectionObjectField
#line 644 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2741 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 73:  // aggregationProjectionObjectFields:
                              // aggregationProjectionObjectFields aggregationProjectionObjectField
#line 648 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 74:  // aggregationProjectionObjectField: idAsProjectionPath
                              // aggregationProjection
#line 656 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 75:  // aggregationProjectionObjectField: aggregationProjectionFieldname
                              // aggregationProjection
#line 659 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2766 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 76:  // match: "object" predicates "end of object"
#line 665 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 77:  // predicates: %empty
#line 671 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2782 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 78:  // predicates: predicates predicate
#line 674 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2791 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 79:  // predicate: predFieldname predValue
#line 680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2799 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 80:  // predicate: logicalExpr
#line 683 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2805 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 81:  // predicate: commentExpr
#line 684 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2811 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 82:  // predValue: simpleValue
#line 691 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2817 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 83:  // predValue: "object" compoundMatchExprs "end of object"
#line 692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2825 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 84:  // compoundMatchExprs: %empty
#line 698 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2833 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 85:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 701 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 86:  // operatorExpression: notExpr
#line 709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2848 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 87:  // operatorExpression: existsExpr
#line 709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2854 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 88:  // operatorExpression: typeExpr
#line 709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2860 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 89:  // existsExpr: EXISTS value
#line 713 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2868 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 90:  // typeArray: "array" typeValues "end of array"
#line 719 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2876 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 91:  // typeValues: %empty
#line 725 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 2882 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 92:  // typeValues: typeValues typeValue
#line 726 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2891 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 93:  // typeValue: num
#line 733 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2897 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 94:  // typeValue: string
#line 733 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2903 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 95:  // typeExpr: TYPE typeValue
#line 737 "src/mongo/db/cst/grammar.yy"
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
#line 2917 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 96:  // typeExpr: TYPE typeArray
#line 746 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 2929 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 97:  // commentExpr: COMMENT value
#line 756 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2937 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 98:  // notExpr: NOT regex
#line 762 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2945 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 99:  // notExpr: NOT "object" compoundMatchExprs operatorExpression "end of
                              // object"
#line 767 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2956 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 100:  // logicalExpr: logicalExprField "array" additionalExprs match "end
                               // of array"
#line 777 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 101:  // logicalExprField: AND
#line 785 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2972 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 102:  // logicalExprField: OR
#line 786 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2978 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 103:  // logicalExprField: NOR
#line 787 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2984 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 104:  // additionalExprs: %empty
#line 790 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2992 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 105:  // additionalExprs: additionalExprs match
#line 793 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 3001 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 106:  // predFieldname: idAsUserFieldname
#line 800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3007 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 107:  // predFieldname: argAsUserFieldname
#line 800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3013 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 108:  // predFieldname: invariableUserFieldname
#line 800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3019 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 109:  // invariableUserFieldname: "fieldname"
#line 803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3027 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 110:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 811 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 3035 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 111:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 814 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 3043 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 112:  // stageAsUserFieldname: STAGE_SKIP
#line 817 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 3051 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 113:  // stageAsUserFieldname: STAGE_LIMIT
#line 820 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 3059 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 114:  // stageAsUserFieldname: STAGE_PROJECT
#line 823 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 3067 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 115:  // stageAsUserFieldname: STAGE_SAMPLE
#line 826 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 3075 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 116:  // argAsUserFieldname: arg
#line 832 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3083 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 117:  // argAsProjectionPath: arg
#line 838 "src/mongo/db/cst/grammar.yy"
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
#line 3098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 118:  // arg: "coll argument"
#line 854 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 3106 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 119:  // arg: "pipeline argument"
#line 857 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 3114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 120:  // arg: "size argument"
#line 860 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 3122 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 121:  // arg: "input argument"
#line 863 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 3130 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 122:  // arg: "to argument"
#line 866 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 3138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 123:  // arg: "onError argument"
#line 869 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 3146 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 124:  // arg: "onNull argument"
#line 872 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 3154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 125:  // arg: "dateString argument"
#line 875 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 3162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 126:  // arg: "format argument"
#line 878 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 3170 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 127:  // arg: "timezone argument"
#line 881 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 3178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 128:  // arg: "date argument"
#line 884 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 3186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 129:  // arg: "chars argument"
#line 887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 3194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 130:  // arg: "regex argument"
#line 890 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 131:  // arg: "options argument"
#line 893 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 132:  // arg: "find argument"
#line 896 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 133:  // arg: "replacement argument"
#line 899 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 134:  // arg: "hour argument"
#line 902 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"hour"};
                    }
#line 3234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 135:  // arg: "year argument"
#line 905 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"year"};
                    }
#line 3242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 136:  // arg: "minute argument"
#line 908 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"minute"};
                    }
#line 3250 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 137:  // arg: "second argument"
#line 911 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"second"};
                    }
#line 3258 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 138:  // arg: "millisecond argument"
#line 914 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"millisecond"};
                    }
#line 3266 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 139:  // arg: "day argument"
#line 917 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"day"};
                    }
#line 3274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 140:  // arg: "ISO day of week argument"
#line 920 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoDayOfWeek"};
                    }
#line 3282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 141:  // arg: "ISO week argument"
#line 923 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeek"};
                    }
#line 3290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 142:  // arg: "ISO week year argument"
#line 926 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeekYear"};
                    }
#line 3298 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 143:  // arg: "ISO 8601 argument"
#line 929 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"iso8601"};
                    }
#line 3306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 144:  // arg: "month argument"
#line 932 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"month"};
                    }
#line 3314 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: ADD
#line 940 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3322 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: ATAN2
#line 943 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: AND
#line 946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3338 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: CONST_EXPR
#line 949 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3346 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: LITERAL
#line 952 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3354 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: OR
#line 955 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3362 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: NOT
#line 958 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3370 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: CMP
#line 961 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3378 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: EQ
#line 964 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3386 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: GT
#line 967 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3394 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: GTE
#line 970 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3402 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: LT
#line 973 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3410 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: LTE
#line 976 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3418 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: NE
#line 979 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3426 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: CONVERT
#line 982 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3434 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: TO_BOOL
#line 985 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3442 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: TO_DATE
#line 988 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3450 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: TO_DECIMAL
#line 991 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3458 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: TO_DOUBLE
#line 994 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3466 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: TO_INT
#line 997 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3474 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: TO_LONG
#line 1000 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3482 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 1003 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3490 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: TO_STRING
#line 1006 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3498 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: TYPE
#line 1009 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3506 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: ABS
#line 1012 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3514 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: CEIL
#line 1015 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3522 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: DIVIDE
#line 1018 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3530 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: EXPONENT
#line 1021 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3538 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: FLOOR
#line 1024 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3546 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: LN
#line 1027 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3554 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: LOG
#line 1030 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3562 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: LOGTEN
#line 1033 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3570 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: MOD
#line 1036 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3578 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: MULTIPLY
#line 1039 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3586 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: POW
#line 1042 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3594 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: ROUND
#line 1045 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3602 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: "slice"
#line 1048 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3610 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: SQRT
#line 1051 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3618 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: SUBTRACT
#line 1054 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3626 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: TRUNC
#line 1057 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3634 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: CONCAT
#line 1060 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3642 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: DATE_FROM_PARTS
#line 1063 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromParts"};
                    }
#line 3650 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: DATE_TO_PARTS
#line 1066 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToParts"};
                    }
#line 3658 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: DAY_OF_MONTH
#line 1069 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfMonth"};
                    }
#line 3666 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: DAY_OF_WEEK
#line 1072 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfWeek"};
                    }
#line 3674 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 190:  // aggExprAsUserFieldname: DAY_OF_YEAR
#line 1075 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfYear"};
                    }
#line 3682 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 191:  // aggExprAsUserFieldname: HOUR
#line 1078 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$hour"};
                    }
#line 3690 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 192:  // aggExprAsUserFieldname: ISO_DAY_OF_WEEK
#line 1081 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoDayOfWeek"};
                    }
#line 3698 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 193:  // aggExprAsUserFieldname: ISO_WEEK
#line 1084 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeek"};
                    }
#line 3706 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 194:  // aggExprAsUserFieldname: ISO_WEEK_YEAR
#line 1087 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeekYear"};
                    }
#line 3714 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 195:  // aggExprAsUserFieldname: MILLISECOND
#line 1090 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$millisecond"};
                    }
#line 3722 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 196:  // aggExprAsUserFieldname: MINUTE
#line 1093 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$minute"};
                    }
#line 3730 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 197:  // aggExprAsUserFieldname: MONTH
#line 1096 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$month"};
                    }
#line 3738 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 198:  // aggExprAsUserFieldname: SECOND
#line 1099 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$second"};
                    }
#line 3746 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 199:  // aggExprAsUserFieldname: WEEK
#line 1102 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$week"};
                    }
#line 3754 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 200:  // aggExprAsUserFieldname: YEAR
#line 1105 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$year"};
                    }
#line 3762 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 201:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 1108 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3770 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 202:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 1111 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3778 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 203:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 1114 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3786 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 204:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 1117 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3794 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 205:  // aggExprAsUserFieldname: LTRIM
#line 1120 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3802 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 206:  // aggExprAsUserFieldname: META
#line 1123 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3810 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 207:  // aggExprAsUserFieldname: REGEX_FIND
#line 1126 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3818 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 208:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 1129 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3826 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 209:  // aggExprAsUserFieldname: REGEX_MATCH
#line 1132 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3834 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 210:  // aggExprAsUserFieldname: REPLACE_ONE
#line 1135 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 211:  // aggExprAsUserFieldname: REPLACE_ALL
#line 1138 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3850 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 212:  // aggExprAsUserFieldname: RTRIM
#line 1141 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3858 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 213:  // aggExprAsUserFieldname: SPLIT
#line 1144 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3866 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 214:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 1147 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3874 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 215:  // aggExprAsUserFieldname: STR_LEN_CP
#line 1150 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3882 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 216:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 1153 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3890 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 217:  // aggExprAsUserFieldname: SUBSTR
#line 1156 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3898 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 218:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 1159 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3906 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 219:  // aggExprAsUserFieldname: SUBSTR_CP
#line 1162 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3914 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 220:  // aggExprAsUserFieldname: TO_LOWER
#line 1165 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3922 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 221:  // aggExprAsUserFieldname: TRIM
#line 1168 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3930 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 222:  // aggExprAsUserFieldname: TO_UPPER
#line 1171 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3938 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 223:  // aggExprAsUserFieldname: "allElementsTrue"
#line 1174 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3946 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 224:  // aggExprAsUserFieldname: "anyElementTrue"
#line 1177 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3954 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 225:  // aggExprAsUserFieldname: "setDifference"
#line 1180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3962 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 226:  // aggExprAsUserFieldname: "setEquals"
#line 1183 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3970 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 227:  // aggExprAsUserFieldname: "setIntersection"
#line 1186 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3978 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 228:  // aggExprAsUserFieldname: "setIsSubset"
#line 1189 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3986 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 229:  // aggExprAsUserFieldname: "setUnion"
#line 1192 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3994 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 230:  // aggExprAsUserFieldname: SIN
#line 1195 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 4002 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 231:  // aggExprAsUserFieldname: COS
#line 1198 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 4010 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 232:  // aggExprAsUserFieldname: TAN
#line 1201 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 4018 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 233:  // aggExprAsUserFieldname: SINH
#line 1204 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 4026 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 234:  // aggExprAsUserFieldname: COSH
#line 1207 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 4034 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 235:  // aggExprAsUserFieldname: TANH
#line 1210 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 4042 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 236:  // aggExprAsUserFieldname: ASIN
#line 1213 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 4050 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 237:  // aggExprAsUserFieldname: ACOS
#line 1216 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 4058 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 238:  // aggExprAsUserFieldname: ATAN
#line 1219 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 4066 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 239:  // aggExprAsUserFieldname: ASINH
#line 1222 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 4074 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 240:  // aggExprAsUserFieldname: ACOSH
#line 1225 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 4082 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 241:  // aggExprAsUserFieldname: ATANH
#line 1228 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 4090 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 242:  // aggExprAsUserFieldname: DEGREES_TO_RADIANS
#line 1231 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 4098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 243:  // aggExprAsUserFieldname: RADIANS_TO_DEGREES
#line 1234 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 4106 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 244:  // string: "string"
#line 1241 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 4114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 245:  // string: "geoNearDistance"
#line 1246 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 4122 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 246:  // string: "geoNearPoint"
#line 1249 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 4130 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 247:  // string: "indexKey"
#line 1252 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 4138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 248:  // string: "randVal"
#line 1255 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 4146 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 249:  // string: "recordId"
#line 1258 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 4154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 250:  // string: "searchHighlights"
#line 1261 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 4162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 251:  // string: "searchScore"
#line 1264 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 4170 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 252:  // string: "sortKey"
#line 1267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 4178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 253:  // string: "textScore"
#line 1270 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 4186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 254:  // aggregationFieldPath: "$-prefixed string"
#line 1276 "src/mongo/db/cst/grammar.yy"
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
#line 4202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 255:  // variable: "$$-prefixed string"
#line 1290 "src/mongo/db/cst/grammar.yy"
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
#line 4218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 256:  // binary: "BinData"
#line 1304 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 4226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 257:  // undefined: "undefined"
#line 1310 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 4234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 258:  // objectId: "ObjectID"
#line 1316 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 4242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 259:  // date: "Date"
#line 1322 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 4250 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 260:  // null: "null"
#line 1328 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 4258 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 261:  // regex: "regex"
#line 1334 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 4266 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 262:  // dbPointer: "dbPointer"
#line 1340 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 4274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 263:  // javascript: "Code"
#line 1346 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 4282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 264:  // symbol: "Symbol"
#line 1352 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 4290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 265:  // javascriptWScope: "CodeWScope"
#line 1358 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 4298 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 266:  // timestamp: "Timestamp"
#line 1364 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 4306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 267:  // minKey: "minKey"
#line 1370 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 4314 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 268:  // maxKey: "maxKey"
#line 1376 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 4322 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 269:  // int: "arbitrary integer"
#line 1382 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 4330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 270:  // int: "zero (int)"
#line 1385 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 4338 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 271:  // int: "1 (int)"
#line 1388 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 4346 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 272:  // int: "-1 (int)"
#line 1391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 4354 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 273:  // long: "arbitrary long"
#line 1397 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 4362 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 274:  // long: "zero (long)"
#line 1400 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 4370 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 275:  // long: "1 (long)"
#line 1403 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 4378 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 276:  // long: "-1 (long)"
#line 1406 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 4386 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 277:  // double: "arbitrary double"
#line 1412 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 4394 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 278:  // double: "zero (double)"
#line 1415 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 4402 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 279:  // double: "1 (double)"
#line 1418 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4410 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 280:  // double: "-1 (double)"
#line 1421 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4418 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 281:  // decimal: "arbitrary decimal"
#line 1427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4426 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 282:  // decimal: "zero (decimal)"
#line 1430 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4434 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 283:  // decimal: "1 (decimal)"
#line 1433 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4442 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 284:  // decimal: "-1 (decimal)"
#line 1436 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4450 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 285:  // bool: "true"
#line 1442 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4458 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 286:  // bool: "false"
#line 1445 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4466 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 287:  // simpleValue: string
#line 1451 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4472 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 288:  // simpleValue: aggregationFieldPath
#line 1452 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4478 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 289:  // simpleValue: variable
#line 1453 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4484 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 290:  // simpleValue: binary
#line 1454 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4490 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 291:  // simpleValue: undefined
#line 1455 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4496 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 292:  // simpleValue: objectId
#line 1456 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 293:  // simpleValue: date
#line 1457 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4508 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 294:  // simpleValue: null
#line 1458 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4514 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 295:  // simpleValue: regex
#line 1459 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4520 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 296:  // simpleValue: dbPointer
#line 1460 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4526 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 297:  // simpleValue: javascript
#line 1461 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4532 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 298:  // simpleValue: symbol
#line 1462 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4538 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 299:  // simpleValue: javascriptWScope
#line 1463 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4544 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 300:  // simpleValue: int
#line 1464 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4550 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 301:  // simpleValue: long
#line 1465 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4556 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 302:  // simpleValue: double
#line 1466 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4562 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 303:  // simpleValue: decimal
#line 1467 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4568 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 304:  // simpleValue: bool
#line 1468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4574 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 305:  // simpleValue: timestamp
#line 1469 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4580 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 306:  // simpleValue: minKey
#line 1470 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4586 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 307:  // simpleValue: maxKey
#line 1471 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4592 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 308:  // expressions: %empty
#line 1478 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 4598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 309:  // expressions: expressions expression
#line 1479 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4607 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 310:  // expression: simpleValue
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4613 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 311:  // expression: expressionObject
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4619 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 312:  // expression: expressionArray
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4625 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 313:  // expression: aggregationOperator
#line 1486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4631 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 314:  // nonArrayExpression: simpleValue
#line 1490 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4637 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 315:  // nonArrayExpression: nonArrayCompoundExpression
#line 1490 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4643 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 316:  // nonArrayNonObjExpression: simpleValue
#line 1494 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4649 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 317:  // nonArrayNonObjExpression: aggregationOperator
#line 1494 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4655 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 318:  // nonArrayCompoundExpression: expressionObject
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4661 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 319:  // nonArrayCompoundExpression: aggregationOperator
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4667 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 320:  // aggregationOperator: aggregationOperatorWithoutSlice
#line 1502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4673 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 321:  // aggregationOperator: slice
#line 1502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4679 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 322:  // aggregationOperatorWithoutSlice: maths
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4685 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 323:  // aggregationOperatorWithoutSlice: boolExprs
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4691 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 324:  // aggregationOperatorWithoutSlice: literalEscapes
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4697 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 325:  // aggregationOperatorWithoutSlice: compExprs
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4703 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 326:  // aggregationOperatorWithoutSlice: typeExpression
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4709 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 327:  // aggregationOperatorWithoutSlice: stringExps
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4715 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 328:  // aggregationOperatorWithoutSlice: setExpression
#line 1506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4721 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 329:  // aggregationOperatorWithoutSlice: trig
#line 1507 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4727 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 330:  // aggregationOperatorWithoutSlice: meta
#line 1507 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4733 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 331:  // aggregationOperatorWithoutSlice: dateExps
#line 1507 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4739 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 332:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1512 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4747 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 333:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1519 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4755 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 334:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1525 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4764 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 335:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1529 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4773 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 336:  // expressionArray: "array" expressions "end of array"
#line 1538 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4781 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 337:  // expressionSingletonArray: "array" expression "end of array"
#line 1545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4789 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 338:  // singleArgExpression: nonArrayExpression
#line 1550 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4795 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 339:  // singleArgExpression: expressionSingletonArray
#line 1550 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4801 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 340:  // expressionObject: "object" expressionFields "end of object"
#line 1555 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4809 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 341:  // expressionFields: %empty
#line 1561 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4817 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 342:  // expressionFields: expressionFields expressionField
#line 1564 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4826 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 343:  // expressionField: expressionFieldname expression
#line 1571 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4834 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 344:  // expressionFieldname: invariableUserFieldname
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4840 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 345:  // expressionFieldname: argAsUserFieldname
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 346:  // expressionFieldname: idAsUserFieldname
#line 1578 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4852 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 347:  // idAsUserFieldname: ID
#line 1582 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4860 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 348:  // elemMatchAsUserFieldname: "elemMatch operator"
#line 1588 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$elemMatch"};
                    }
#line 4868 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 349:  // idAsProjectionPath: ID
#line 1594 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 4876 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 350:  // maths: add
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4882 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 351:  // maths: abs
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4888 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 352:  // maths: ceil
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4894 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 353:  // maths: divide
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4900 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 354:  // maths: exponent
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4906 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 355:  // maths: floor
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4912 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 356:  // maths: ln
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4918 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 357:  // maths: log
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4924 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 358:  // maths: logten
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4930 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 359:  // maths: mod
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4936 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 360:  // maths: multiply
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4942 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 361:  // maths: pow
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4948 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 362:  // maths: round
#line 1600 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4954 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 363:  // maths: sqrt
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 364:  // maths: subtract
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 365:  // maths: trunc
#line 1601 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4972 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 366:  // meta: "object" META "geoNearDistance" "end of object"
#line 1605 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4980 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 367:  // meta: "object" META "geoNearPoint" "end of object"
#line 1608 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4988 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 368:  // meta: "object" META "indexKey" "end of object"
#line 1611 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4996 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 369:  // meta: "object" META "randVal" "end of object"
#line 1614 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 5004 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 370:  // meta: "object" META "recordId" "end of object"
#line 1617 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 5012 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 371:  // meta: "object" META "searchHighlights" "end of object"
#line 1620 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 5020 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 372:  // meta: "object" META "searchScore" "end of object"
#line 1623 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 5028 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 373:  // meta: "object" META "sortKey" "end of object"
#line 1626 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 5036 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 374:  // meta: "object" META "textScore" "end of object"
#line 1629 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 5044 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 375:  // trig: sin
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5050 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 376:  // trig: cos
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5056 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 377:  // trig: tan
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5062 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 378:  // trig: sinh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5068 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 379:  // trig: cosh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5074 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 380:  // trig: tanh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5080 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 381:  // trig: asin
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5086 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 382:  // trig: acos
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5092 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 383:  // trig: atan
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5098 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 384:  // trig: atan2
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5104 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 385:  // trig: asinh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5110 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 386:  // trig: acosh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5116 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 387:  // trig: atanh
#line 1634 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5122 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 388:  // trig: degreesToRadians
#line 1635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5128 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 389:  // trig: radiansToDegrees
#line 1635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5134 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 390:  // add: "object" ADD expressionArray "end of object"
#line 1639 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5143 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 391:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1646 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5152 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 392:  // abs: "object" ABS singleArgExpression "end of object"
#line 1652 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5160 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 393:  // ceil: "object" CEIL singleArgExpression "end of object"
#line 1657 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 394:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1662 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5177 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 395:  // exponent: "object" EXPONENT singleArgExpression "end of object"
#line 1668 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5185 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 396:  // floor: "object" FLOOR singleArgExpression "end of object"
#line 1673 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5193 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 397:  // ln: "object" LN singleArgExpression "end of object"
#line 1678 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5201 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 398:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1683 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 399:  // logten: "object" LOGTEN singleArgExpression "end of object"
#line 1689 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 400:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1694 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5227 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 401:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1700 "src/mongo/db/cst/grammar.yy"
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
#line 5239 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 402:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1709 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5248 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 403:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1715 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5257 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 404:  // sqrt: "object" SQRT singleArgExpression "end of object"
#line 1721 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5265 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 405:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1726 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 406:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1732 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5283 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 407:  // sin: "object" SIN singleArgExpression "end of object"
#line 1738 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5291 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 408:  // cos: "object" COS singleArgExpression "end of object"
#line 1743 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5299 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 409:  // tan: "object" TAN singleArgExpression "end of object"
#line 1748 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5307 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 410:  // sinh: "object" SINH singleArgExpression "end of object"
#line 1753 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5315 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 411:  // cosh: "object" COSH singleArgExpression "end of object"
#line 1758 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5323 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 412:  // tanh: "object" TANH singleArgExpression "end of object"
#line 1763 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5331 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 413:  // asin: "object" ASIN singleArgExpression "end of object"
#line 1768 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5339 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 414:  // acos: "object" ACOS singleArgExpression "end of object"
#line 1773 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5347 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 415:  // atan: "object" ATAN singleArgExpression "end of object"
#line 1778 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5355 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 416:  // asinh: "object" ASINH singleArgExpression "end of object"
#line 1783 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5363 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 417:  // acosh: "object" ACOSH singleArgExpression "end of object"
#line 1788 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5371 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 418:  // atanh: "object" ATANH singleArgExpression "end of object"
#line 1793 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5379 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 419:  // degreesToRadians: "object" DEGREES_TO_RADIANS singleArgExpression
                               // "end of object"
#line 1798 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5387 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 420:  // radiansToDegrees: "object" RADIANS_TO_DEGREES singleArgExpression
                               // "end of object"
#line 1803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5395 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 421:  // boolExprs: and
#line 1809 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5401 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 422:  // boolExprs: or
#line 1809 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5407 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 423:  // boolExprs: not
#line 1809 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5413 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 424:  // and: "object" AND expressionArray "end of object"
#line 1813 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 425:  // or: "object" OR expressionArray "end of object"
#line 1820 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5431 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 426:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1827 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5440 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 427:  // stringExps: concat
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 428:  // stringExps: dateFromString
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5452 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 429:  // stringExps: dateToString
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5458 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 430:  // stringExps: indexOfBytes
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5464 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 431:  // stringExps: indexOfCP
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 432:  // stringExps: ltrim
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5476 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 433:  // stringExps: regexFind
#line 1834 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5482 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 434:  // stringExps: regexFindAll
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5488 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 435:  // stringExps: regexMatch
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 436:  // stringExps: replaceOne
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5500 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 437:  // stringExps: replaceAll
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5506 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 438:  // stringExps: rtrim
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5512 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 439:  // stringExps: split
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5518 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 440:  // stringExps: strLenBytes
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 441:  // stringExps: strLenCP
#line 1835 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5530 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 442:  // stringExps: strcasecmp
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5536 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 443:  // stringExps: substr
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 444:  // stringExps: substrBytes
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5548 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 445:  // stringExps: substrCP
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5554 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 446:  // stringExps: toLower
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5560 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 447:  // stringExps: trim
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5566 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 448:  // stringExps: toUpper
#line 1836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5572 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 449:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1840 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5584 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 450:  // formatArg: %empty
#line 1850 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5592 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 451:  // formatArg: "format argument" expression
#line 1853 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5600 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 452:  // timezoneArg: %empty
#line 1859 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5608 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 453:  // timezoneArg: "timezone argument" expression
#line 1862 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5616 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 454:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1870 "src/mongo/db/cst/grammar.yy"
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
#line 5626 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 455:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1879 "src/mongo/db/cst/grammar.yy"
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
#line 5636 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 456:  // dateExps: dateFromParts
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5642 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 457:  // dateExps: dateToParts
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5648 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 458:  // dateExps: dayOfMonth
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 459:  // dateExps: dayOfWeek
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5660 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 460:  // dateExps: dayOfYear
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5666 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 461:  // dateExps: hour
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5672 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 462:  // dateExps: isoDayOfWeek
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5678 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 463:  // dateExps: isoWeek
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5684 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 464:  // dateExps: isoWeekYear
#line 1887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5690 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 465:  // dateExps: millisecond
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5696 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 466:  // dateExps: minute
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5702 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 467:  // dateExps: month
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5708 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 468:  // dateExps: second
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5714 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 469:  // dateExps: week
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5720 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 470:  // dateExps: year
#line 1888 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 471:  // hourArg: %empty
#line 1892 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
                    }
#line 5734 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 472:  // hourArg: "hour argument" expression
#line 1895 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::hourArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5742 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 473:  // minuteArg: %empty
#line 1901 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
                    }
#line 5750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 474:  // minuteArg: "minute argument" expression
#line 1904 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::minuteArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 475:  // secondArg: %empty
#line 1910 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
                    }
#line 5766 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 476:  // secondArg: "second argument" expression
#line 1913 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::secondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 477:  // millisecondArg: %empty
#line 1919 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
                    }
#line 5782 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 478:  // millisecondArg: "millisecond argument" expression
#line 1922 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::millisecondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5790 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 479:  // dayArg: %empty
#line 1928 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
                    }
#line 5798 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 480:  // dayArg: "day argument" expression
#line 1931 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 481:  // isoDayOfWeekArg: %empty
#line 1937 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5814 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 482:  // isoDayOfWeekArg: "ISO day of week argument" expression
#line 1940 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoDayOfWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 483:  // isoWeekArg: %empty
#line 1946 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5830 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 484:  // isoWeekArg: "ISO week argument" expression
#line 1949 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5838 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 485:  // iso8601Arg: %empty
#line 1955 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
                    }
#line 5846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 486:  // iso8601Arg: "ISO 8601 argument" bool
#line 1958 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::iso8601Arg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5854 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 487:  // monthArg: %empty
#line 1964 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
                    }
#line 5862 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 488:  // monthArg: "month argument" expression
#line 1967 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::monthArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5870 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 489:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg millisecondArg minuteArg monthArg secondArg
                               // timezoneArg "year argument" expression "end of object" "end of
                               // object"
#line 1974 "src/mongo/db/cst/grammar.yy"
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
#line 5880 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 490:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg isoDayOfWeekArg isoWeekArg "ISO week year argument"
                               // expression millisecondArg minuteArg monthArg secondArg timezoneArg
                               // "end of object" "end of object"
#line 1980 "src/mongo/db/cst/grammar.yy"
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
#line 5890 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 491:  // dateToParts: "object" DATE_TO_PARTS START_ORDERED_OBJECT "date
                               // argument" expression iso8601Arg timezoneArg "end of object" "end
                               // of object"
#line 1988 "src/mongo/db/cst/grammar.yy"
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
#line 5900 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 492:  // dayOfMonth: "object" DAY_OF_MONTH nonArrayNonObjExpression "end of
                               // object"
#line 1996 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5908 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 493:  // dayOfMonth: "object" DAY_OF_MONTH START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 1999 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5917 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 494:  // dayOfMonth: "object" DAY_OF_MONTH expressionSingletonArray "end of
                               // object"
#line 2003 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5925 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 495:  // dayOfWeek: "object" DAY_OF_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 2009 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5934 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 496:  // dayOfWeek: "object" DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2013 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5943 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 497:  // dayOfWeek: "object" DAY_OF_WEEK expressionSingletonArray "end of
                               // object"
#line 2017 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5951 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 498:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK nonArrayNonObjExpression
                               // "end of object"
#line 2023 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 499:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2027 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5969 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 500:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK expressionSingletonArray
                               // "end of object"
#line 2031 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5977 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 501:  // dayOfYear: "object" DAY_OF_YEAR nonArrayNonObjExpression "end of
                               // object"
#line 2037 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5986 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 502:  // dayOfYear: "object" DAY_OF_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2041 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5995 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 503:  // dayOfYear: "object" DAY_OF_YEAR expressionSingletonArray "end of
                               // object"
#line 2045 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6003 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 504:  // hour: "object" HOUR nonArrayNonObjExpression "end of object"
#line 2051 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6012 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 505:  // hour: "object" HOUR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2055 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6021 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 506:  // hour: "object" HOUR expressionSingletonArray "end of object"
#line 2059 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6029 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 507:  // month: "object" MONTH nonArrayNonObjExpression "end of object"
#line 2065 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6038 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 508:  // month: "object" MONTH START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2069 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6047 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 509:  // month: "object" MONTH expressionSingletonArray "end of object"
#line 2073 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6055 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 510:  // week: "object" WEEK nonArrayNonObjExpression "end of object"
#line 2079 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6064 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 511:  // week: "object" WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2083 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6073 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 512:  // week: "object" WEEK expressionSingletonArray "end of object"
#line 2087 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6081 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 513:  // isoWeek: "object" ISO_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 2093 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6090 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 514:  // isoWeek: "object" ISO_WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2097 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6099 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 515:  // isoWeek: "object" ISO_WEEK expressionSingletonArray "end of
                               // object"
#line 2101 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6107 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 516:  // isoWeekYear: "object" ISO_WEEK_YEAR nonArrayNonObjExpression "end
                               // of object"
#line 2107 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6116 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 517:  // isoWeekYear: "object" ISO_WEEK_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2111 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6125 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 518:  // isoWeekYear: "object" ISO_WEEK_YEAR expressionSingletonArray "end
                               // of object"
#line 2115 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6133 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 519:  // year: "object" YEAR nonArrayNonObjExpression "end of object"
#line 2121 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6142 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 520:  // year: "object" YEAR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2125 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6151 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 521:  // year: "object" YEAR expressionSingletonArray "end of object"
#line 2129 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6159 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 522:  // second: "object" SECOND nonArrayNonObjExpression "end of object"
#line 2135 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 523:  // second: "object" SECOND START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2139 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6177 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 524:  // second: "object" SECOND expressionSingletonArray "end of object"
#line 2143 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6185 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 525:  // millisecond: "object" MILLISECOND nonArrayNonObjExpression "end of
                               // object"
#line 2149 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 526:  // millisecond: "object" MILLISECOND START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2153 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6203 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 527:  // millisecond: "object" MILLISECOND expressionSingletonArray "end of
                               // object"
#line 2157 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6211 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 528:  // minute: "object" MINUTE nonArrayNonObjExpression "end of object"
#line 2163 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6220 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 529:  // minute: "object" MINUTE START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2167 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6229 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 530:  // minute: "object" MINUTE expressionSingletonArray "end of object"
#line 2171 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6237 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 531:  // exprZeroToTwo: %empty
#line 2177 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 6245 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 532:  // exprZeroToTwo: expression
#line 2180 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6253 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 533:  // exprZeroToTwo: expression expression
#line 2183 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6261 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 534:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 2190 "src/mongo/db/cst/grammar.yy"
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
#line 6273 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 535:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 2201 "src/mongo/db/cst/grammar.yy"
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
#line 6285 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 536:  // charsArg: %empty
#line 2211 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 6293 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 537:  // charsArg: "chars argument" expression
#line 2214 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6301 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 538:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2220 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6311 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 539:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2228 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6321 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 540:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 2236 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6331 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 541:  // optionsArg: %empty
#line 2244 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 6339 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 542:  // optionsArg: "options argument" expression
#line 2247 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6347 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 543:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 2252 "src/mongo/db/cst/grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 6359 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 544:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 2261 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6367 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 545:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 2267 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 546:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 2273 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6383 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 547:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2280 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6394 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 548:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2290 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6405 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 549:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 2299 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6414 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 550:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 2306 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6423 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 551:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 2313 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6432 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 552:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 2321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6441 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 553:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 2329 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6450 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 554:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 2337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6459 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 555:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 2345 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6468 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 556:  // toLower: "object" TO_LOWER expression "end of object"
#line 2352 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6476 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 557:  // toUpper: "object" TO_UPPER expression "end of object"
#line 2358 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6484 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 558:  // metaSortKeyword: "randVal"
#line 2364 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 6492 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 559:  // metaSortKeyword: "textScore"
#line 2367 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 6500 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 560:  // metaSort: "object" META metaSortKeyword "end of object"
#line 2373 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6508 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 561:  // sortSpecs: "object" specList "end of object"
#line 2379 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6516 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 562:  // specList: %empty
#line 2384 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 563:  // specList: specList sortSpec
#line 2387 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6533 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 564:  // oneOrNegOne: "1 (int)"
#line 2394 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 6541 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 565:  // oneOrNegOne: "-1 (int)"
#line 2397 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 6549 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 566:  // oneOrNegOne: "1 (long)"
#line 2400 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 6557 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 567:  // oneOrNegOne: "-1 (long)"
#line 2403 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 6565 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 568:  // oneOrNegOne: "1 (double)"
#line 2406 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 6573 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 569:  // oneOrNegOne: "-1 (double)"
#line 2409 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 6581 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 570:  // oneOrNegOne: "1 (decimal)"
#line 2412 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 6589 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 571:  // oneOrNegOne: "-1 (decimal)"
#line 2415 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 6597 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 572:  // sortFieldname: valueFieldname
#line 2420 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 6605 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 573:  // sortFieldname: "fieldname containing dotted path"
#line 2422 "src/mongo/db/cst/grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 6617 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 574:  // sortSpec: sortFieldname metaSort
#line 2432 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6625 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 575:  // sortSpec: sortFieldname oneOrNegOne
#line 2434 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6633 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 576:  // findProject: "object" findProjectFields "end of object"
#line 2440 "src/mongo/db/cst/grammar.yy"
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
#line 6654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 577:  // findProjectFields: %empty
#line 2459 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6662 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 578:  // findProjectFields: findProjectFields findProjectField
#line 2462 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6671 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 579:  // findProjectField: ID topLevelFindProjection
#line 2469 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6679 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 580:  // findProjectField: projectionFieldname topLevelFindProjection
#line 2472 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6687 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 581:  // topLevelFindProjection: findProjection
#line 2478 "src/mongo/db/cst/grammar.yy"
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
#line 6703 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 582:  // findProjection: projectionCommon
#line 2492 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6709 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 583:  // findProjection: findProjectionObject
#line 2493 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6715 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 584:  // findProjection: aggregationOperatorWithoutSlice
#line 2494 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6721 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 585:  // findProjection: findProjectionSlice
#line 2495 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6727 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 586:  // findProjection: elemMatch
#line 2496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6733 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 587:  // elemMatch: "object" "elemMatch operator" match "end of object"
#line 2500 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = {CNode::ObjectChildren{
                            {KeyFieldname::elemMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6741 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 588:  // findProjectionSlice: "object" "slice" num "end of object"
#line 2506 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6749 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 589:  // findProjectionSlice: "object" "slice" "array" num num "end of
                               // array" "end of object"
#line 2509 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6758 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 590:  // findProjectionObject: "object" findProjectionObjectFields "end of
                               // object"
#line 2517 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6766 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 591:  // findProjectionObjectFields: findProjectionObjectField
#line 2524 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6775 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 592:  // findProjectionObjectFields: findProjectionObjectFields
                               // findProjectionObjectField
#line 2528 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6784 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 593:  // findProjectionObjectField: idAsProjectionPath findProjection
#line 2536 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 594:  // findProjectionObjectField: projectionFieldname findProjection
#line 2539 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6800 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 595:  // setExpression: allElementsTrue
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6806 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 596:  // setExpression: anyElementTrue
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6812 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 597:  // setExpression: setDifference
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6818 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 598:  // setExpression: setEquals
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6824 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 599:  // setExpression: setIntersection
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6830 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 600:  // setExpression: setIsSubset
#line 2545 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6836 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 601:  // setExpression: setUnion
#line 2546 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6842 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 602:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 2550 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6850 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 603:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 2556 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6858 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 604:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 2562 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6867 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 605:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2570 "src/mongo/db/cst/grammar.yy"
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
#line 6879 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 606:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 2581 "src/mongo/db/cst/grammar.yy"
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
#line 6891 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 607:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 2591 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6900 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 608:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2599 "src/mongo/db/cst/grammar.yy"
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
#line 6912 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 609:  // literalEscapes: const
#line 2609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6918 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 610:  // literalEscapes: literal
#line 2609 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6924 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 611:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 2613 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6933 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 612:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 2620 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6942 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 613:  // value: simpleValue
#line 2627 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6948 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 614:  // value: compoundValue
#line 2627 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6954 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 615:  // compoundValue: valueArray
#line 2631 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 616:  // compoundValue: valueObject
#line 2631 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6966 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 617:  // valueArray: "array" values "end of array"
#line 2635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 6974 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 618:  // values: %empty
#line 2641 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 6980 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 619:  // values: values value
#line 2642 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 6989 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 620:  // valueObject: "object" valueFields "end of object"
#line 2649 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6997 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 621:  // valueFields: %empty
#line 2655 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 7005 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 622:  // valueFields: valueFields valueField
#line 2658 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 7014 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 623:  // valueField: valueFieldname value
#line 2665 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7022 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 624:  // valueFieldname: invariableUserFieldname
#line 2672 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7028 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 625:  // valueFieldname: stageAsUserFieldname
#line 2673 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7034 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 626:  // valueFieldname: argAsUserFieldname
#line 2674 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7040 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 627:  // valueFieldname: aggExprAsUserFieldname
#line 2675 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7046 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 628:  // valueFieldname: idAsUserFieldname
#line 2676 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7052 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 629:  // valueFieldname: elemMatchAsUserFieldname
#line 2677 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 7058 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 630:  // compExprs: cmp
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7064 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 631:  // compExprs: eq
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7070 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 632:  // compExprs: gt
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7076 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 633:  // compExprs: gte
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7082 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 634:  // compExprs: lt
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7088 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 635:  // compExprs: lte
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7094 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 636:  // compExprs: ne
#line 2680 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7100 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 637:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 2682 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7109 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 638:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 2687 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7118 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 639:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 2692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7127 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 640:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 2697 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7136 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 641:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 2702 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7145 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 642:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 2707 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 643:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 2712 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7163 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 644:  // typeExpression: convert
#line 2718 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7169 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 645:  // typeExpression: toBool
#line 2719 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7175 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 646:  // typeExpression: toDate
#line 2720 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7181 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 647:  // typeExpression: toDecimal
#line 2721 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7187 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 648:  // typeExpression: toDouble
#line 2722 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7193 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 649:  // typeExpression: toInt
#line 2723 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7199 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 650:  // typeExpression: toLong
#line 2724 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7205 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 651:  // typeExpression: toObjectId
#line 2725 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7211 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 652:  // typeExpression: toString
#line 2726 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7217 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 653:  // typeExpression: type
#line 2727 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 7223 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 654:  // onErrorArg: %empty
#line 2732 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 7231 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 655:  // onErrorArg: "onError argument" expression
#line 2735 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7239 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 656:  // onNullArg: %empty
#line 2742 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 7247 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 657:  // onNullArg: "onNull argument" expression
#line 2745 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7255 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 658:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2752 "src/mongo/db/cst/grammar.yy"
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
#line 7266 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 659:  // toBool: "object" TO_BOOL expression "end of object"
#line 2761 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 660:  // toDate: "object" TO_DATE expression "end of object"
#line 2766 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 661:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2771 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 662:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 2776 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7298 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 663:  // toInt: "object" TO_INT expression "end of object"
#line 2781 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 664:  // toLong: "object" TO_LONG expression "end of object"
#line 2786 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7314 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 665:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 2791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7322 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 666:  // toString: "object" TO_STRING expression "end of object"
#line 2796 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 667:  // type: "object" TYPE expression "end of object"
#line 2801 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7338 "src/mongo/db/cst/parser_gen.cpp"
                    break;


#line 7342 "src/mongo/db/cst/parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -1035;

const short ParserGen::yytable_ninf_ = -482;

const short ParserGen::yypact_[] = {
    -123,  -92,   -88,   -86,   -83,   61,    -77,   -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, 44,    4,     208,   273,   1542,  -64,   986,   -39,   -36,   986,   -22,
    21,    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, 3518,  -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, 4110,  -1035, -1035, -1035, -1035, -66,   -1035,
    4258,  -1035, -1035, 4258,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, 185,   -1035, -1035, -1035, -1035, 53,    -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, 94,    -1035, -1035, 118,   -77,   -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, 1709,  -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, 34,    -1035, -1035, -1035, 784,   986,   235,   -1035,
    2334,  2043,  -35,   -88,   2482,  3666,  3666,  3666,  -3,    -2,    -3,    -1,    3666,
    3666,  3666,  5,     3666,  3666,  5,     15,    16,    -22,   3666,  3666,  -22,   -22,
    -22,   -22,   3814,  3814,  3814,  3666,  17,    -88,   5,     3666,  3666,  5,     5,
    3814,  -1035, 23,    24,    3814,  3814,  3814,  29,    3666,  32,    3666,  5,     5,
    -22,   305,   3814,  3814,  33,    3814,  35,    5,     46,    -3,    47,    3666,  -22,
    -22,   -22,   -22,   -22,   49,    -22,   3814,  5,     50,    55,    5,     56,    682,
    3666,  3666,  58,    3666,  62,    3962,  3962,  63,    64,    65,    66,    3666,  3666,
    3962,  3962,  3962,  3962,  3962,  3962,  3962,  3962,  3962,  3962,  -22,   67,    3962,
    3814,  3814,  4258,  4258,  944,   -1035, -45,   -1035, 4406,  4406,  -1035, -1035, 72,
    106,   -1035, -1035, -1035, 3518,  -1035, -1035, 3518,  -96,   1253,  -1035, -1035, -1035,
    -1035, 76,    -1035, 2209,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, 3962,  -1035,
    -1035, -1035, -1035, -1035, -1035, 80,    93,    125,   133,   3962,  140,   3962,  141,
    144,   148,   3962,  178,   180,   183,   186,   -1035, 3518,  156,   187,   188,   244,
    246,   248,   249,   2209,  -1035, -1035, 193,   194,   252,   196,   197,   255,   199,
    202,   260,   204,   3962,  205,   206,   207,   209,   212,   213,   243,   245,   304,
    3962,  3962,  250,   251,   310,   254,   256,   314,   259,   261,   317,   3518,  262,
    3962,  263,   264,   268,   328,   270,   271,   272,   274,   275,   277,   283,   285,
    286,   289,   295,   354,   300,   301,   362,   3962,  306,   309,   367,   3962,  313,
    3962,  316,   3962,  320,   321,   364,   322,   323,   377,   378,   3962,  328,   326,
    327,   385,   330,   3962,  3962,  332,   3962,  986,   333,   334,   335,   3962,  336,
    3962,  338,   339,   3962,  3962,  3962,  3962,  340,   343,   344,   345,   346,   347,
    350,   351,   352,   353,   355,   356,   328,   3962,  359,   360,   361,   411,   365,
    370,   424,   -1035, -1035, -1035, -1035, -1035, -1035, 371,   1876,  -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -34,   -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, 302,   1440,  376,   -1035, -1035, -1035, -1035, 379,   -1035, 380,   -1035,
    -1035, -1035, 3962,  -1035, -1035, -1035, -1035, 2630,  381,   3962,  -1035, -1035, 3962,
    427,   3962,  3962,  3962,  -1035, -1035, 3962,  -1035, -1035, 3962,  -1035, -1035, 3962,
    -1035, 3962,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, 3962,  3962,  3962,
    -1035, -1035, 3962,  -1035, -1035, 3962,  -1035, -1035, 3962,  382,   -1035, 3962,  -1035,
    -1035, -1035, 3962,  433,   -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, 3962,  -1035, -1035, 3962,  3962,  -1035, -1035, 3962,  3962,  -1035, 387,
    -1035, 3962,  -1035, -1035, 3962,  -1035, -1035, 3962,  3962,  3962,  434,   -1035, -1035,
    3962,  -1035, 3962,  3962,  -1035, 3962,  986,   -1035, -1035, -1035, 3962,  -1035, 3962,
    -1035, -1035, 3962,  3962,  3962,  3962,  -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, 435,   3962,  -1035, -1035, -1035, 3962,  -1035, -1035,
    3962,  -1035, 4406,  4406,  -1035, 1114,  389,   -44,   4448,  3962,  390,   391,   -1035,
    3962,  -1035, -1035, -1035, -1035, -1035, 392,   393,   395,   396,   397,   419,   -1035,
    3962,  91,    447,   448,   447,   432,   432,   432,   401,   432,   3962,  3962,  432,
    432,   432,   402,   404,   -1035, 3962,  432,   432,   405,   432,   -1035, 406,   408,
    441,   455,   458,   410,   3962,  432,   -1035, -1035, -1035, 412,   413,   414,   3962,
    3962,  3962,  415,   3962,  416,   432,   432,   -1035, -1035, -1035, -1035, -1035, 417,
    -1035, -1035, 3962,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, 3962,  451,
    -1035, 3962,  3962,  460,   465,   3962,  432,   37,    432,   432,   3962,  420,   421,
    422,   423,   437,   3962,  425,   453,   478,   479,   482,   -1035, 483,   484,   485,
    486,   487,   490,   2778,  -1035, 496,   3962,  457,   3962,  3962,  512,   515,   521,
    2926,  3074,  3222,  522,   523,   524,   454,   459,   529,   531,   532,   533,   534,
    537,   -1035, 3370,  -1035, 3962,  571,   -1035, -1035, 3962,  582,   3962,  586,   -1035,
    419,   -1035, 545,   451,   -1035, 546,   547,   550,   -1035, 551,   -1035, 553,   554,
    555,   559,   561,   -1035, 563,   564,   565,   -1035, 566,   569,   -1035, -1035, 3962,
    608,   609,   -1035, 575,   577,   578,   579,   580,   -1035, -1035, -1035, 581,   585,
    590,   -1035, 593,   -1035, 595,   596,   583,   -1035, 3962,  -1035, 3962,  622,   -1035,
    3962,  451,   597,   600,   -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, 604,   3962,  3962,  -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, 606,   -1035, 3962,  432,   652,   616,
    -1035, 624,   -1035, 630,   631,   632,   -1035, 651,   460,   633,   -1035, 635,   640,
    -1035, 3962,  582,   -1035, -1035, -1035, 642,   622,   643,   432,   -1035, 644,   645,
    -1035};

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
    310, 309, 321, 312, 311, 313, 320, 0,   314, 318, 338, 315, 319, 339, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   308, 0,   0,   0,   0,   479, 0,   0,   0,
    9,   316, 317, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   536, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   536, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   536, 0,   0,   0,   0,   0,   0,
    0,   0,   594, 593, 590, 592, 558, 559, 0,   0,   28,  30,  31,  32,  33,  29,  16,  0,   623,
    89,  84,  98,  91,  94,  96,  95,  93,  100, 0,   0,   0,   392, 414, 417, 390, 0,   424, 0,
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
    -1035, 265,   20,    -1035, -1035, -9,    -1035, -1035, -5,    -1035, 3,     -1035, -720,
    266,   -1035, -1035, -200,  -1035, -1035, 0,     -63,   -42,   -38,   -30,   -20,   -28,
    -19,   -21,   -15,   -26,   -18,   -408,  -67,   -1035, -4,    6,     8,     -289,  10,
    18,    -52,   127,   -1035, -1035, -1035, -1035, -1035, -1035, -193,  -1035, 497,   -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, 159,   -815,  -545,  -1035, -14,
    386,   -448,  -1035, -1035, -65,   4137,  -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -412,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -338,  -1034, -211,  -795,  -658,  -1035, -1035, -435,  -446,  -423,  -1035, -1035, -1035,
    -439,  -1035, -597,  -1035, -213,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, -1035, -1035, -1035, -367,  -54,   -149,  1936,  158,   -396,  -1035, -25,
    -1035, -1035, -1035, -1035, -182,  -1035, -1035, -1035, -1035, -1035, -1035, -1035, -1035,
    657,   -436,  -1035, -1035, -1035, -1035, -1035, 176,   -1035, -1035, -1035, -1035, -1035,
    -1035, -1035, 198};

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
    421,  422, 423, 424,  425,  426,  427,  428, 429,  430, 431,  599,  600,  594, 595,  602,  603,
    631,  9,   17,  457,  287,  458,  73,   74,  582,  583, 584,  585,  75,   76,  918,  11,   18,
    433,  434, 435, 436,  437,  563,  84,   564, 13,   19,  448,  449,  749,  201, 5,    694};

const short ParserGen::yytable_[] = {
    221,  219,  220,  221,  219,  220,  222,  223,  68,   222,  226,  316,  69,   324,  316,  309,
    324,  72,   309,  72,   70,   787,  753,  753,  432,  617,  322,  432,  579,  322,  913,  206,
    207,  208,  578,  1121, 310,  579,  81,   310,  311,  642,  868,  311,  645,  646,  761,  574,
    312,  6,    313,  312,  314,  313,  8,    314,  10,   665,  666,  12,   315,  14,   586,  315,
    580,  15,   688,  747,  1,    2,    3,    4,    33,   580,  317,  288,  902,  317,  202,  704,
    230,  231,  707,  251,  318,  1156, 319,  318,  320,  319,  229,  320,  641,  601,  601,  601,
    321,  982,  983,  321,  601,  601,  601,  224,  601,  601,  225,  748,  695,  696,  601,  601,
    1002, -481, -481, 1003, 629,  629,  629,  601,  227,  581,  450,  601,  601,  743,  744,  629,
    452,  453,  581,  629,  629,  629,  565,  601,  758,  601,  303,  607,  609,  757,  629,  629,
    768,  629,  613,  1013, 1014, 772,  1016, 601,  217,  1020, 1021, 1022, 618,  619,  640,  629,
    1026, 1027, 773,  1029, 650,  651,  601,  601,  759,  601,  661,  760,  1039, 663,  683,  789,
    687,  601,  601,  20,   21,   22,   23,   24,   25,   1052, 1053, 689,  691,  280,  699,  705,
    629,  629,  774,  913,  706,  708,  286,  713,  755,  755,  775,  715,  718,  719,  720,  721,
    735,  777,  779,  788,  1066, 780,  1068, 1069, 34,   781,  35,   36,   37,   38,   39,   228,
    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,
    56,   57,   58,   59,   60,   439,  440,  783,  61,   784,  441,  442,  785,  828,  62,   786,
    790,  791,  792,  794,  795,  796,  797,  798,  799,  800,  801,  802,  803,  443,  444,  804,
    805,  806,  808,  809,  810,  63,   811,  445,  446,  812,  813,  35,   36,   37,   38,   39,
    64,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   57,   58,   59,   60,   232,  233,  814,  61,   815,  65,   816,  66,   234,  819,
    820,  920,  821,  822,  561,  823,  824,  447,  825,  827,  826,  829,  831,  832,  630,  633,
    636,  833,  834,  836,  837,  838,  77,   839,  840,  647,  841,  235,  236,  652,  655,  658,
    842,  78,   843,  844,  237,  238,  845,  1164, 677,  680,  1030, 684,  846,  239,  847,  753,
    753,  848,  849,  1040, 1041, 1042, 850,  852,  67,   701,  853,  854,  668,  669,  856,  862,
    1181, 858,  572,  242,  670,  860,  861,  863,  864,  865,  866,  869,  870,  871,  606,  872,
    608,  875,  878,  879,  880,  882,  243,  884,  885,  890,  737,  740,  891,  892,  893,  894,
    895,  671,  672,  896,  897,  898,  899,  907,  900,  901,  673,  674,  904,  905,  906,  221,
    219,  220,  908,  675,  910,  222,  571,  909,  911,  79,   80,   919,  928,  936,  999,  929,
    930,  933,  951,  690,  954,  966,  978,  960,  72,   676,  986,  991,  992,  994,  995,  996,
    1006, 997,  998,  1011, 1008, 1015, 1033, 1023, 1024, 1028, 1035, 1031, 1032, 1036, 1037, 1057,
    1043, 1044, 1045, 1049, 1051, 1061, 1054, 1063, 1091, 1071, 1072, 1073, 1074, 1077, 316,  316,
    324,  324,  309,  309,  316,  316,  324,  324,  309,  309,  1075, 432,  432,  322,  322,  221,
    219,  220,  764,  322,  322,  222,  710,  310,  310,  1078, 1103, 311,  311,  310,  310,  1104,
    762,  311,  311,  312,  312,  313,  313,  314,  314,  312,  312,  313,  313,  314,  314,  315,
    315,  755,  755,  1079, 1080, 315,  315,  1081, 1082, 1083, 1084, 1085, 1086, 317,  317,  1087,
    221,  219,  220,  317,  317,  1089, 222,  318,  318,  319,  319,  320,  320,  318,  318,  319,
    319,  320,  320,  321,  321,  1094, 280,  561,  1095, 321,  321,  597,  597,  597,  1096, 1100,
    1101, 1102, 597,  597,  597,  1105, 597,  597,  1106, 1107, 1108, 1109, 597,  597,  1110, 1113,
    1115, 1118, 628,  628,  628,  597,  1120, 1122, 1123, 597,  597,  1124, 1125, 628,  1126, 1127,
    1128, 628,  628,  628,  1129, 597,  1130, 597,  1131, 1132, 1133, 1134, 628,  628,  1135, 628,
    1137, 1138, 634,  637,  1139, 597,  1140, 1141, 1142, 1143, 1144, 1150, 648,  628,  1145, 1153,
    653,  656,  659,  1146, 597,  597,  1147, 597,  1148, 1149, 1157, 678,  681,  1158, 685,  597,
    597,  1159, 620,  1162, 1003, 623,  624,  625,  626,  632,  635,  638,  702,  1166, 628,  628,
    221,  219,  220,  1171, 649,  1167, 222,  877,  654,  657,  660,  1168, 1169, 1170, 1173, 280,
    1174, 667,  280,  679,  682,  1175, 686,  1178, 1180, 1182, 1183, 985,  568,  738,  741,  1067,
    697,  698,  576,  700,  703,  989,  454,  756,  1119, 1010, 1172, 1179, 1165, 1177, 1019, 987,
    917,  916,  438,  746,  0,    0,    203,  204,  205,  0,    280,  206,  207,  208,  0,    0,
    0,    0,    734,  0,    0,    739,  742,  0,    0,    924,  0,    0,    0,    925,  209,  210,
    211,  0,    72,   0,    0,    926,  0,    0,    212,  213,  214,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    280,  0,    0,    0,    0,    0,    35,   36,   37,   38,   39,   0,
    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,
    56,   57,   58,   59,   60,   0,    0,    709,  61,   0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    316,  316,  324,
    324,  309,  309,  764,  0,    566,  0,    0,    221,  219,  220,  0,    322,  322,  222,  971,
    567,  0,    215,  216,  217,  218,  310,  310,  0,    0,    311,  311,  0,    0,    0,    0,
    0,    0,    312,  312,  313,  313,  314,  314,  0,    0,    0,    0,    0,    0,    315,  315,
    0,    221,  219,  220,  0,    0,    0,    222,  0,    0,    0,    0,    317,  317,  0,    0,
    0,    0,    0,    0,    0,    0,    318,  318,  319,  319,  320,  320,  716,  717,  0,    0,
    0,    0,    321,  321,  724,  725,  726,  727,  728,  729,  730,  731,  732,  733,  0,    0,
    736,  0,    0,    0,    0,    0,    0,    0,    79,   80,   35,   36,   37,   38,   39,   0,
    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,
    56,   57,   58,   59,   60,   0,    771,  0,    61,   0,    0,    0,    0,    0,    0,    0,
    0,    776,  0,    778,  0,    0,    0,    782,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    745,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    495,  0,    807,  0,    0,    0,    0,    0,    0,    0,    0,    0,    817,  818,  0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  830,  0,    206,  207,  208,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    851,  209,  210,
    211,  855,  0,    857,  0,    859,  0,    0,    212,  213,  214,  0,    0,    867,  0,    0,
    0,    0,    0,    873,  874,  0,    876,  0,    0,    0,    0,    881,  0,    883,  0,    0,
    886,  887,  888,  889,  0,    0,    0,    0,    79,   80,   0,    0,    0,    0,    0,    0,
    0,    903,  0,    0,    35,   36,   37,   38,   39,   0,    40,   41,   42,   43,   44,   45,
    46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   0,
    0,    0,    61,   0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    931,  0,    215,  216,  217,  218,  0,    934,  0,    0,    935,  0,    938,  939,  940,  984,
    0,    941,  0,    0,    942,  0,    0,    943,  0,    944,  495,  0,    0,    0,    0,    0,
    0,    0,    945,  946,  947,  0,    0,    948,  0,    0,    949,  0,    0,    950,  0,    0,
    952,  0,    0,    0,    953,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    955,  0,    0,    956,  957,  0,    0,    958,  959,  0,    0,    0,    961,  0,    0,
    962,  0,    0,    963,  964,  965,  0,    0,    0,    967,  0,    968,  969,  0,    970,  0,
    0,    0,    0,    972,  0,    973,  0,    0,    974,  975,  976,  977,  0,    0,    0,    0,
    0,    0,    79,   80,   0,    0,    0,    0,    0,    979,  0,    0,    0,    980,  0,    0,
    981,  0,    0,    0,    0,    0,    0,    0,    0,    990,  0,    0,    0,    993,  0,    0,
    0,    203,  204,  205,  0,    0,    206,  207,  208,  0,    1001, 0,    0,    0,    0,    0,
    232,  233,  0,    0,    1017, 1017, 0,    0,    234,  209,  210,  211,  1025, 0,    0,    0,
    0,    0,    0,    212,  213,  214,  0,    0,    1038, 0,    0,    0,    0,    0,    0,    0,
    1046, 1047, 1048, 0,    1050, 235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,
    1055, 0,    0,    0,    0,    0,    0,    239,  0,    1056, 0,    0,    1059, 1060, 0,    0,
    1065, 0,    763,  0,    0,    1070, 0,    0,    0,    0,    0,    1076, 0,    242,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1090, 0,    1092, 1093, 0,
    243,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    215,  216,  217,
    218,  1111, 0,    1112, 0,    0,    0,    1114, 0,    1117, 35,   36,   37,   38,   39,   0,
    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,
    56,   57,   58,   59,   60,   1136, 0,    0,    61,   0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    1151, 0,    1152, 0,    0,
    1155, 0,    0,    0,    0,    922,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    64,   0,    0,    1160, 1161, 0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    1163, 0,    0,    0,    0,    0,    85,   86,   87,   88,   89,   90,   91,
    35,   36,   37,   38,   39,   1176, 40,   41,   42,   43,   44,   45,   46,   47,   48,   49,
    50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,   92,   93,   94,   61,   95,
    96,   0,    0,    97,   0,    98,   99,   100,  101,  102,  103,  104,  105,  106,  107,  108,
    109,  110,  0,    0,    0,    111,  112,  0,    67,   0,    0,    113,  114,  115,  0,    116,
    117,  0,    0,    118,  119,  120,  64,   121,  122,  0,    0,    0,    0,    123,  124,  125,
    126,  127,  128,  129,  0,    0,    0,    130,  131,  132,  133,  134,  135,  136,  137,  138,
    139,  0,    140,  141,  142,  143,  0,    0,    144,  145,  146,  147,  148,  149,  150,  0,
    0,    151,  152,  153,  154,  155,  156,  157,  0,    158,  159,  160,  161,  162,  163,  164,
    165,  166,  167,  0,    0,    168,  169,  170,  171,  172,  173,  174,  175,  176,  0,    177,
    178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  191,  67,   192,
    460,  461,  462,  463,  464,  465,  466,  35,   36,   37,   38,   39,   0,    40,   41,   42,
    43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,
    59,   60,   467,  468,  469,  61,   470,  471,  0,    0,    472,  0,    473,  474,  475,  476,
    477,  478,  479,  480,  481,  482,  483,  484,  485,  0,    0,    0,    486,  487,  0,    0,
    0,    0,    0,    488,  489,  0,    490,  491,  0,    0,    492,  493,  494,  495,  496,  497,
    0,    0,    0,    0,    498,  499,  500,  501,  502,  503,  504,  0,    0,    0,    505,  506,
    507,  508,  509,  510,  511,  512,  513,  514,  0,    515,  516,  517,  518,  0,    0,    519,
    520,  521,  522,  523,  524,  525,  0,    0,    526,  527,  528,  529,  530,  531,  532,  0,
    533,  534,  535,  536,  0,    0,    0,    0,    0,    0,    0,    0,    537,  538,  539,  540,
    541,  542,  543,  544,  545,  0,    546,  547,  548,  549,  550,  551,  552,  553,  554,  555,
    556,  557,  558,  559,  560,  79,   80,   460,  461,  462,  463,  464,  465,  466,  35,   36,
    37,   38,   39,   0,    40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,
    52,   53,   54,   55,   56,   57,   58,   59,   60,   467,  468,  469,  61,   470,  471,  0,
    0,    472,  0,    473,  474,  475,  476,  477,  478,  479,  480,  481,  482,  483,  484,  485,
    0,    0,    0,    486,  487,  0,    0,    0,    0,    0,    0,    489,  0,    490,  491,  0,
    0,    492,  493,  494,  495,  496,  497,  0,    0,    0,    0,    498,  499,  500,  501,  502,
    503,  504,  0,    0,    0,    505,  506,  507,  508,  509,  510,  511,  512,  513,  514,  0,
    515,  516,  517,  518,  0,    0,    519,  520,  521,  522,  523,  524,  525,  0,    0,    526,
    527,  528,  529,  530,  531,  769,  0,    533,  534,  535,  536,  0,    0,    0,    0,    0,
    0,    0,    0,    537,  538,  539,  540,  541,  542,  543,  544,  545,  0,    546,  547,  548,
    549,  550,  551,  552,  553,  554,  555,  556,  557,  558,  559,  560,  79,   80,   85,   86,
    87,   88,   89,   90,   91,   35,   36,   37,   38,   39,   0,    40,   41,   42,   43,   44,
    45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   60,
    92,   93,   94,   61,   95,   96,   0,    0,    97,   0,    98,   99,   100,  101,  102,  103,
    104,  105,  106,  107,  108,  109,  110,  0,    0,    0,    111,  112,  0,    0,    0,    0,
    575,  114,  115,  0,    116,  117,  0,    0,    118,  119,  120,  64,   121,  122,  0,    0,
    0,    0,    123,  124,  125,  126,  127,  128,  129,  0,    0,    0,    130,  131,  132,  133,
    134,  135,  136,  137,  138,  139,  0,    140,  141,  142,  143,  0,    0,    144,  145,  146,
    147,  148,  149,  150,  0,    0,    151,  152,  153,  154,  155,  156,  157,  0,    158,  159,
    160,  161,  162,  163,  164,  165,  166,  167,  0,    0,    168,  169,  170,  171,  172,  173,
    174,  175,  176,  0,    177,  178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,
    189,  190,  191,  67,   460,  461,  462,  463,  464,  465,  466,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    467,  468,  469,  0,    470,  471,  0,    0,    472,  0,
    473,  474,  475,  476,  477,  478,  479,  480,  481,  482,  483,  484,  485,  0,    0,    0,
    486,  487,  0,    0,    0,    0,    0,    0,    489,  0,    490,  491,  0,    0,    492,  493,
    494,  0,    496,  497,  0,    0,    0,    0,    498,  499,  500,  501,  502,  503,  504,  0,
    0,    0,    505,  506,  507,  508,  509,  510,  511,  512,  513,  514,  0,    515,  516,  517,
    518,  0,    0,    519,  520,  521,  522,  523,  524,  525,  0,    0,    526,  527,  528,  529,
    530,  531,  769,  0,    533,  534,  535,  536,  0,    0,    0,    0,    0,    0,    0,    0,
    537,  538,  539,  540,  541,  542,  543,  544,  545,  0,    546,  547,  548,  549,  550,  551,
    552,  553,  554,  555,  556,  557,  558,  559,  560,  230,  231,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,  205,  604,  605,  206,
    207,  208,  573,  610,  611,  612,  0,    615,  616,  232,  233,  0,    0,    621,  622,  0,
    0,    234,  209,  210,  211,  0,    639,  0,    0,    0,    643,  644,  212,  213,  214,  0,
    0,    0,    0,    0,    0,    0,    662,  0,    664,  0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    692,  237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    711,  712,  0,    714,  0,    0,    240,  241,  0,    0,    0,
    722,  723,  0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  587,  0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,
    588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  932,  0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  1088, 0,
    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,
    211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,
    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,
    254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,
    207,  208,  1097, 0,    0,    0,    0,    0,    0,    232,  233,  0,    0,    0,    0,    0,
    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,    212,  213,  214,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    235,  236,
    0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,    0,    0,
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,  588,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  1098, 0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,
    588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  1099, 0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    303,  588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    243,  244,  245,  246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,
    217,  218,  256,  257,  258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  996,  0,
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
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    240,  241,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  230,  231,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    203,  204,
    205,  0,    0,    206,  207,  208,  0,    0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,    0,    0,    0,    0,
    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    596,
    588,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,
    258,  230,  231,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    203,  204,  205,  0,    0,    206,  207,  208,  0,    0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  209,  210,  211,  0,    0,    0,
    0,    0,    0,    0,    212,  213,  214,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,
    238,  0,    0,    0,    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    596,  627,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
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
    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    285,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  244,  245,  246,  247,  248,  249,
    250,  251,  252,  253,  254,  255,  215,  216,  217,  218,  256,  257,  258,  289,  290,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    291,  292,
    293,  0,    0,    294,  295,  296,  0,    0,    0,    0,    0,    0,    0,    232,  233,  0,
    0,    0,    0,    0,    0,    234,  297,  298,  299,  0,    0,    0,    0,    0,    0,    0,
    300,  301,  302,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,
    0,    0,    0,    0,    239,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    303,
    304,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    243,  0,    0,
    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  305,  306,  307,  308,  256,  257,
    258,  289,  290,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    291,  292,  293,  0,    0,    294,  295,  296,  0,    0,    0,    0,    0,    0,
    0,    232,  233,  0,    0,    0,    0,    0,    0,    234,  297,  298,  299,  0,    0,    0,
    0,    0,    0,    0,    300,  301,  302,  0,    0,    0,    0,    0,    203,  204,  205,  0,
    0,    206,  207,  208,  988,  0,    235,  236,  0,    0,    0,    232,  233,  0,    0,    237,
    238,  0,    0,    234,  209,  210,  211,  0,    239,  0,    0,    0,    0,    0,    212,  213,
    214,  0,    0,    303,  750,  0,    0,    0,    0,    0,    0,    0,    0,    0,    242,  0,
    235,  236,  0,    0,    0,    0,    0,    0,    0,    237,  238,  0,    0,    0,    0,    0,
    0,    243,  239,  0,    246,  247,  248,  249,  250,  251,  252,  253,  254,  255,  305,  306,
    307,  308,  256,  257,  258,  598,  598,  598,  242,  0,    0,    0,    598,  598,  598,  0,
    598,  598,  0,    0,    0,    0,    598,  598,  0,    0,    0,    243,  0,    0,    0,    598,
    0,    0,    0,    598,  598,  0,    0,    0,    215,  216,  217,  218,  0,    0,    0,    598,
    0,    598,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    598,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    598,  598,
    0,    598,  0,    0,    0,    0,    0,    0,    0,    598,  598};

const short ParserGen::yycheck_[] = {
    21,   21,   21,  24,   24,   24,   21,   21,   17,   24,   24,   78,   17,   78,   81,   78,
    81,   17,   81,  19,   17,   618,  567,  568,  78,   473,  78,   81,   72,   81,   750,  65,
    66,   67,   69,  1069, 78,   72,   18,   81,   78,   489,  700,  81,   492,  493,  142,  455,
    78,   141,  78,  81,   78,   81,   142,  81,   142,  505,  506,  142,  78,   0,    458,  81,
    108,  142,  514, 112,  191,  192,  193,  194,  68,   108,  78,   141,  734,  81,   142,  527,
    43,   44,   530, 179,  78,   1119, 78,   81,   78,   81,   69,   81,   488,  460,  461,  462,
    78,   912,  913, 81,   467,  468,  469,  142,  471,  472,  142,  152,  520,  521,  477,  478,
    21,   22,   23,  24,   483,  484,  485,  486,  142,  165,  69,   490,  491,  561,  562,  494,
    34,   11,   165, 498,  499,  500,  100,  502,  30,   504,  141,  141,  141,  69,   509,  510,
    68,   512,  141, 942,  943,  69,   945,  518,  186,  948,  949,  950,  141,  141,  141,  526,
    955,  956,  69,  958,  141,  141,  533,  534,  576,  536,  141,  579,  967,  141,  141,  19,
    141,  544,  545, 135,  136,  137,  138,  139,  140,  980,  981,  141,  141,  62,   141,  141,
    559,  560,  69,  915,  141,  141,  71,   141,  567,  568,  69,   141,  141,  141,  141,  141,
    141,  69,   69,  619,  1007, 69,   1009, 1010, 8,    69,   10,   11,   12,   13,   14,   25,
    16,   17,   18,  19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,
    32,   33,   34,  35,   36,   60,   61,   69,   40,   69,   65,   66,   69,   661,  46,   69,
    69,   69,   14,  13,   12,   12,   69,   69,   12,   69,   69,   12,   69,   84,   85,   69,
    12,   69,   69,  69,   69,   69,   69,   94,   95,   69,   69,   10,   11,   12,   13,   14,
    80,   16,   17,  18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,
    31,   32,   33,  34,   35,   36,   75,   76,   69,   40,   69,   107,  12,   109,  83,   69,
    69,   769,  12,  69,   304,  69,   12,   142,  69,   12,   69,   69,   69,   69,   483,  484,
    485,  69,   10,  69,   69,   69,   69,   69,   69,   494,  69,   112,  113,  498,  499,  500,
    69,   80,   69,  69,   121,  122,  69,   1154, 509,  510,  959,  512,  69,   130,  12,   912,
    913,  69,   69,  968,  969,  970,  12,   69,   168,  526,  69,   12,   75,   76,   69,   19,
    1179, 69,   453, 152,  83,   69,   69,   69,   69,   16,   16,   69,   69,   12,   463,  69,
    465,  69,   69,  69,   69,   69,   171,  69,   69,   69,   559,  560,  69,   69,   69,   69,
    69,   112,  113, 69,   69,   69,   69,   12,   69,   69,   121,  122,  69,   69,   69,   452,
    452,  452,  69,  130,  12,   452,  452,  69,   69,   168,  169,  141,  68,   18,   27,   68,
    68,   68,   68,  516,  19,   19,   19,   68,   456,  152,  69,   69,   69,   69,   69,   68,
    17,   69,   69,  35,   20,   68,   29,   69,   68,   68,   19,   69,   68,   19,   68,   28,
    68,   68,   68,  68,   68,   25,   69,   22,   31,   69,   69,   69,   69,   68,   561,  562,
    561,  562,  561, 562,  567,  568,  567,  568,  567,  568,  69,   561,  562,  561,  562,  532,
    532,  532,  581, 567,  568,  532,  532,  561,  562,  68,   68,   561,  562,  567,  568,  68,
    580,  567,  568, 561,  562,  561,  562,  561,  562,  567,  568,  567,  568,  567,  568,  561,
    562,  912,  913, 69,   69,   567,  568,  69,   69,   69,   69,   69,   69,   561,  562,  69,
    581,  581,  581, 567,  568,  69,   581,  561,  562,  561,  562,  561,  562,  567,  568,  567,
    568,  567,  568, 561,  562,  69,   455,  563,  69,   567,  568,  460,  461,  462,  69,   69,
    69,   69,   467, 468,  469,  68,   471,  472,  69,   69,   69,   69,   477,  478,  69,   36,
    26,   23,   483, 484,  485,  486,  69,   69,   69,   490,  491,  69,   69,   494,  69,   69,
    69,   498,  499, 500,  69,   502,  69,   504,  69,   69,   69,   69,   509,  510,  69,   512,
    32,   32,   484, 485,  69,   518,  69,   69,   69,   69,   69,   68,   494,  526,  69,   33,
    498,  499,  500, 69,   533,  534,  69,   536,  69,   69,   69,   509,  510,  69,   512,  544,
    545,  69,   476, 69,   24,   479,  480,  481,  482,  483,  484,  485,  526,  69,   559,  560,
    709,  709,  709, 40,   494,  69,   709,  709,  498,  499,  500,  69,   69,   69,   69,   576,
    69,   507,  579, 509,  510,  69,   512,  69,   69,   69,   69,   915,  451,  559,  560,  1008,
    522,  523,  456, 525,  526,  918,  229,  568,  1066, 940,  1165, 1177, 1155, 1172, 947,  917,
    761,  758,  81,  563,  -1,   -1,   60,   61,   62,   -1,   619,  65,   66,   67,   -1,   -1,
    -1,   -1,   556, -1,   -1,   559,  560,  -1,   -1,   770,  -1,   -1,   -1,   770,  84,   85,
    86,   -1,   770, -1,   -1,   770,  -1,   -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   661,  -1,   -1,   -1,   -1,   -1,   10,   11,   12,   13,   14,   -1,
    16,   17,   18,  19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,
    32,   33,   34,  35,   36,   -1,   -1,   141,  40,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   912,  913,  912,
    913,  912,  913, 918,  -1,   69,   -1,   -1,   877,  877,  877,  -1,   912,  913,  877,  877,
    80,   -1,   184, 185,  186,  187,  912,  913,  -1,   -1,   912,  913,  -1,   -1,   -1,   -1,
    -1,   -1,   912, 913,  912,  913,  912,  913,  -1,   -1,   -1,   -1,   -1,   -1,   912,  913,
    -1,   918,  918, 918,  -1,   -1,   -1,   918,  -1,   -1,   -1,   -1,   912,  913,  -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   912,  913,  912,  913,  912,  913,  538,  539,  -1,   -1,
    -1,   -1,   912, 913,  546,  547,  548,  549,  550,  551,  552,  553,  554,  555,  -1,   -1,
    558,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   168,  169,  10,   11,   12,   13,   14,   -1,
    16,   17,   18,  19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,
    32,   33,   34,  35,   36,   -1,   596,  -1,   40,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   607,  -1,  609,  -1,   -1,   -1,   613,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   69,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    80,   -1,   640, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   650,  651,  -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   663,  -1,   65,   66,   67,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   683,  84,   85,
    86,   687,  -1,  689,  -1,   691,  -1,   -1,   94,   95,   96,   -1,   -1,   699,  -1,   -1,
    -1,   -1,   -1,  705,  706,  -1,   708,  -1,   -1,   -1,   -1,   713,  -1,   715,  -1,   -1,
    718,  719,  720, 721,  -1,   -1,   -1,   -1,   168,  169,  -1,   -1,   -1,   -1,   -1,   -1,
    -1,   735,  -1,  -1,   10,   11,   12,   13,   14,   -1,   16,   17,   18,   19,   20,   21,
    22,   23,   24,  25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,   -1,
    -1,   -1,   40,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    782,  -1,   184, 185,  186,  187,  -1,   789,  -1,   -1,   792,  -1,   794,  795,  796,  69,
    -1,   799,  -1,  -1,   802,  -1,   -1,   805,  -1,   807,  80,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   816, 817,  818,  -1,   -1,   821,  -1,   -1,   824,  -1,   -1,   827,  -1,   -1,
    830,  -1,   -1,  -1,   834,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   847,  -1,  -1,   850,  851,  -1,   -1,   854,  855,  -1,   -1,   -1,   859,  -1,   -1,
    862,  -1,   -1,  865,  866,  867,  -1,   -1,   -1,   871,  -1,   873,  874,  -1,   876,  -1,
    -1,   -1,   -1,  881,  -1,   883,  -1,   -1,   886,  887,  888,  889,  -1,   -1,   -1,   -1,
    -1,   -1,   168, 169,  -1,   -1,   -1,   -1,   -1,   903,  -1,   -1,   -1,   907,  -1,   -1,
    910,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   919,  -1,   -1,   -1,   923,  -1,   -1,
    -1,   60,   61,  62,   -1,   -1,   65,   66,   67,   -1,   936,  -1,   -1,   -1,   -1,   -1,
    75,   76,   -1,  -1,   946,  947,  -1,   -1,   83,   84,   85,   86,   954,  -1,   -1,   -1,
    -1,   -1,   -1,  94,   95,   96,   -1,   -1,   966,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    974,  975,  976, -1,   978,  112,  113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,  122,
    990,  -1,   -1,  -1,   -1,   -1,   -1,   130,  -1,   999,  -1,   -1,   1002, 1003, -1,   -1,
    1006, -1,   141, -1,   -1,   1011, -1,   -1,   -1,   -1,   -1,   1017, -1,   152,  -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   1033, -1,   1035, 1036, -1,
    171,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   184,  185,  186,
    187,  1055, -1,  1057, -1,   -1,   -1,   1061, -1,   1063, 10,   11,   12,   13,   14,   -1,
    16,   17,   18,  19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,
    32,   33,   34,  35,   36,   1091, -1,   -1,   40,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   1113, -1,   1115, -1,   -1,
    1118, -1,   -1,  -1,   -1,   69,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    80,   -1,   -1,  1137, 1138, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  1153, -1,   -1,   -1,   -1,   -1,   3,    4,    5,    6,    7,    8,    9,
    10,   11,   12,  13,   14,   1171, 16,   17,   18,   19,   20,   21,   22,   23,   24,   25,
    26,   27,   28,  29,   30,   31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,
    42,   -1,   -1,  45,   -1,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,
    58,   59,   -1,  -1,   -1,   63,   64,   -1,   168,  -1,   -1,   69,   70,   71,   -1,   73,
    74,   -1,   -1,  77,   78,   79,   80,   81,   82,   -1,   -1,   -1,   -1,   87,   88,   89,
    90,   91,   92,  93,   -1,   -1,   -1,   97,   98,   99,   100,  101,  102,  103,  104,  105,
    106,  -1,   108, 109,  110,  111,  -1,   -1,   114,  115,  116,  117,  118,  119,  120,  -1,
    -1,   123,  124, 125,  126,  127,  128,  129,  -1,   131,  132,  133,  134,  135,  136,  137,
    138,  139,  140, -1,   -1,   143,  144,  145,  146,  147,  148,  149,  150,  151,  -1,   153,
    154,  155,  156, 157,  158,  159,  160,  161,  162,  163,  164,  165,  166,  167,  168,  169,
    3,    4,    5,   6,    7,    8,    9,    10,   11,   12,   13,   14,   -1,   16,   17,   18,
    19,   20,   21,  22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,
    35,   36,   37,  38,   39,   40,   41,   42,   -1,   -1,   45,   -1,   47,   48,   49,   50,
    51,   52,   53,  54,   55,   56,   57,   58,   59,   -1,   -1,   -1,   63,   64,   -1,   -1,
    -1,   -1,   -1,  70,   71,   -1,   73,   74,   -1,   -1,   77,   78,   79,   80,   81,   82,
    -1,   -1,   -1,  -1,   87,   88,   89,   90,   91,   92,   93,   -1,   -1,   -1,   97,   98,
    99,   100,  101, 102,  103,  104,  105,  106,  -1,   108,  109,  110,  111,  -1,   -1,   114,
    115,  116,  117, 118,  119,  120,  -1,   -1,   123,  124,  125,  126,  127,  128,  129,  -1,
    131,  132,  133, 134,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   143,  144,  145,  146,
    147,  148,  149, 150,  151,  -1,   153,  154,  155,  156,  157,  158,  159,  160,  161,  162,
    163,  164,  165, 166,  167,  168,  169,  3,    4,    5,    6,    7,    8,    9,    10,   11,
    12,   13,   14,  -1,   16,   17,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,
    28,   29,   30,  31,   32,   33,   34,   35,   36,   37,   38,   39,   40,   41,   42,   -1,
    -1,   45,   -1,  47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   57,   58,   59,
    -1,   -1,   -1,  63,   64,   -1,   -1,   -1,   -1,   -1,   -1,   71,   -1,   73,   74,   -1,
    -1,   77,   78,  79,   80,   81,   82,   -1,   -1,   -1,   -1,   87,   88,   89,   90,   91,
    92,   93,   -1,  -1,   -1,   97,   98,   99,   100,  101,  102,  103,  104,  105,  106,  -1,
    108,  109,  110, 111,  -1,   -1,   114,  115,  116,  117,  118,  119,  120,  -1,   -1,   123,
    124,  125,  126, 127,  128,  129,  -1,   131,  132,  133,  134,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  143,  144,  145,  146,  147,  148,  149,  150,  151,  -1,   153,  154,  155,
    156,  157,  158, 159,  160,  161,  162,  163,  164,  165,  166,  167,  168,  169,  3,    4,
    5,    6,    7,   8,    9,    10,   11,   12,   13,   14,   -1,   16,   17,   18,   19,   20,
    21,   22,   23,  24,   25,   26,   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
    37,   38,   39,  40,   41,   42,   -1,   -1,   45,   -1,   47,   48,   49,   50,   51,   52,
    53,   54,   55,  56,   57,   58,   59,   -1,   -1,   -1,   63,   64,   -1,   -1,   -1,   -1,
    69,   70,   71,  -1,   73,   74,   -1,   -1,   77,   78,   79,   80,   81,   82,   -1,   -1,
    -1,   -1,   87,  88,   89,   90,   91,   92,   93,   -1,   -1,   -1,   97,   98,   99,   100,
    101,  102,  103, 104,  105,  106,  -1,   108,  109,  110,  111,  -1,   -1,   114,  115,  116,
    117,  118,  119, 120,  -1,   -1,   123,  124,  125,  126,  127,  128,  129,  -1,   131,  132,
    133,  134,  135, 136,  137,  138,  139,  140,  -1,   -1,   143,  144,  145,  146,  147,  148,
    149,  150,  151, -1,   153,  154,  155,  156,  157,  158,  159,  160,  161,  162,  163,  164,
    165,  166,  167, 168,  3,    4,    5,    6,    7,    8,    9,    -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   37,   38,   39,   -1,   41,   42,   -1,   -1,   45,   -1,
    47,   48,   49,  50,   51,   52,   53,   54,   55,   56,   57,   58,   59,   -1,   -1,   -1,
    63,   64,   -1,  -1,   -1,   -1,   -1,   -1,   71,   -1,   73,   74,   -1,   -1,   77,   78,
    79,   -1,   81,  82,   -1,   -1,   -1,   -1,   87,   88,   89,   90,   91,   92,   93,   -1,
    -1,   -1,   97,  98,   99,   100,  101,  102,  103,  104,  105,  106,  -1,   108,  109,  110,
    111,  -1,   -1,  114,  115,  116,  117,  118,  119,  120,  -1,   -1,   123,  124,  125,  126,
    127,  128,  129, -1,   131,  132,  133,  134,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    143,  144,  145, 146,  147,  148,  149,  150,  151,  -1,   153,  154,  155,  156,  157,  158,
    159,  160,  161, 162,  163,  164,  165,  166,  167,  43,   44,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,   461,  462,  65,
    66,   67,   68,  467,  468,  469,  -1,   471,  472,  75,   76,   -1,   -1,   477,  478,  -1,
    -1,   83,   84,  85,   86,   -1,   486,  -1,   -1,   -1,   490,  491,  94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   502,  -1,   504,  -1,   -1,   -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   518,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   533,  534,  -1,   536,  -1,   -1,   141,  142,  -1,   -1,   -1,
    544,  545,  -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   68,   -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   68,   -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,   66,   67,   68,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176,  177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,
    66,   67,   68,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   68,   -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   68,   -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,   66,   67,   68,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176,  177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,
    66,   67,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,  142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,
    122,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   171,  172, 173,  174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,   66,   67,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,
    86,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,  -1,   -1,   -1,   -1,
    -1,   -1,   -1,  121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   130,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   152, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   171,  172,  173,  174,  175,  176,  177,  178,  179,  180,  181,
    182,  183,  184, 185,  186,  187,  188,  189,  190,  43,   44,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,
    66,   67,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,   -1,   -1,   -1,   -1,
    -1,   83,   84,  85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   94,   95,   96,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   112,  113,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,   -1,   -1,   -1,   -1,
    130,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   142,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   171,  172,  173,  174,  175,  176,  177,
    178,  179,  180, 181,  182,  183,  184,  185,  186,  187,  188,  189,  190,  43,   44,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,
    62,   -1,   -1,  65,   66,   67,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   75,   76,   -1,
    -1,   -1,   -1,  -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    94,   95,   96,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   112, 113,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,
    -1,   -1,   -1,  -1,   130,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   141,
    142,  -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,   -1,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   171,  -1,   -1,
    174,  175,  176, 177,  178,  179,  180,  181,  182,  183,  184,  185,  186,  187,  188,  189,
    190,  43,   44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   -1,   60,  61,   62,   -1,   -1,   65,   66,   67,   -1,   -1,   -1,   -1,   -1,   -1,
    -1,   75,   76,  -1,   -1,   -1,   -1,   -1,   -1,   83,   84,   85,   86,   -1,   -1,   -1,
    -1,   -1,   -1,  -1,   94,   95,   96,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,   -1,
    -1,   65,   66,  67,   68,   -1,   112,  113,  -1,   -1,   -1,   75,   76,   -1,   -1,   121,
    122,  -1,   -1,  83,   84,   85,   86,   -1,   130,  -1,   -1,   -1,   -1,   -1,   94,   95,
    96,   -1,   -1,  141,  142,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   152,  -1,
    112,  113,  -1,  -1,   -1,   -1,   -1,   -1,   -1,   121,  122,  -1,   -1,   -1,   -1,   -1,
    -1,   171,  130, -1,   174,  175,  176,  177,  178,  179,  180,  181,  182,  183,  184,  185,
    186,  187,  188, 189,  190,  460,  461,  462,  152,  -1,   -1,   -1,   467,  468,  469,  -1,
    471,  472,  -1,  -1,   -1,   -1,   477,  478,  -1,   -1,   -1,   171,  -1,   -1,   -1,   486,
    -1,   -1,   -1,  490,  491,  -1,   -1,   -1,   184,  185,  186,  187,  -1,   -1,   -1,   502,
    -1,   504,  -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   518,
    -1,   -1,   -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   533,  534,
    -1,   536,  -1,  -1,   -1,   -1,   -1,   -1,   -1,   544,  545};

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
    236, 260, 263, 264, 265, 395, 396, 141, 236, 265, 393, 394, 395, 397, 398, 398, 398, 264, 141,
    264, 141, 398, 398, 398, 141, 261, 398, 398, 261, 141, 141, 431, 398, 398, 431, 431, 431, 431,
    142, 236, 395, 397, 399, 431, 397, 399, 431, 397, 399, 431, 398, 141, 400, 261, 398, 398, 261,
    261, 397, 399, 431, 141, 141, 397, 399, 431, 397, 399, 431, 397, 399, 431, 141, 398, 141, 398,
    261, 261, 431, 75,  76,  83,  112, 113, 121, 122, 130, 152, 397, 399, 431, 397, 399, 431, 141,
    397, 399, 431, 141, 261, 141, 264, 141, 398, 287, 431, 287, 287, 431, 431, 141, 431, 397, 399,
    431, 261, 141, 141, 261, 141, 141, 259, 398, 398, 141, 398, 141, 260, 260, 141, 141, 141, 141,
    398, 398, 260, 260, 260, 260, 260, 260, 260, 260, 260, 260, 431, 141, 260, 397, 399, 431, 397,
    399, 431, 417, 417, 69,  423, 112, 152, 428, 142, 255, 256, 257, 258, 395, 255, 69,  30,  226,
    226, 142, 235, 141, 227, 242, 243, 259, 68,  129, 266, 260, 69,  69,  69,  69,  260, 69,  260,
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


#line 57 "src/mongo/db/cst/grammar.yy"
}  // namespace mongo
#line 9428 "src/mongo/db/cst/parser_gen.cpp"

#line 2805 "src/mongo/db/cst/grammar.yy"
