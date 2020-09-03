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

        case symbol_kind::S_dbPointer:                         // dbPointer
        case symbol_kind::S_javascript:                        // javascript
        case symbol_kind::S_symbol:                            // symbol
        case symbol_kind::S_javascriptWScope:                  // javascriptWScope
        case symbol_kind::S_int:                               // int
        case symbol_kind::S_timestamp:                         // timestamp
        case symbol_kind::S_long:                              // long
        case symbol_kind::S_double:                            // double
        case symbol_kind::S_decimal:                           // decimal
        case symbol_kind::S_minKey:                            // minKey
        case symbol_kind::S_maxKey:                            // maxKey
        case symbol_kind::S_value:                             // value
        case symbol_kind::S_string:                            // string
        case symbol_kind::S_aggregationFieldPath:              // aggregationFieldPath
        case symbol_kind::S_binary:                            // binary
        case symbol_kind::S_undefined:                         // undefined
        case symbol_kind::S_objectId:                          // objectId
        case symbol_kind::S_bool:                              // bool
        case symbol_kind::S_date:                              // date
        case symbol_kind::S_null:                              // null
        case symbol_kind::S_regex:                             // regex
        case symbol_kind::S_simpleValue:                       // simpleValue
        case symbol_kind::S_compoundValue:                     // compoundValue
        case symbol_kind::S_valueArray:                        // valueArray
        case symbol_kind::S_valueObject:                       // valueObject
        case symbol_kind::S_valueFields:                       // valueFields
        case symbol_kind::S_variable:                          // variable
        case symbol_kind::S_typeArray:                         // typeArray
        case symbol_kind::S_typeValue:                         // typeValue
        case symbol_kind::S_pipeline:                          // pipeline
        case symbol_kind::S_stageList:                         // stageList
        case symbol_kind::S_stage:                             // stage
        case symbol_kind::S_inhibitOptimization:               // inhibitOptimization
        case symbol_kind::S_unionWith:                         // unionWith
        case symbol_kind::S_skip:                              // skip
        case symbol_kind::S_limit:                             // limit
        case symbol_kind::S_project:                           // project
        case symbol_kind::S_sample:                            // sample
        case symbol_kind::S_projectFields:                     // projectFields
        case symbol_kind::S_projectionObjectFields:            // projectionObjectFields
        case symbol_kind::S_topLevelProjection:                // topLevelProjection
        case symbol_kind::S_projection:                        // projection
        case symbol_kind::S_projectionObject:                  // projectionObject
        case symbol_kind::S_num:                               // num
        case symbol_kind::S_expression:                        // expression
        case symbol_kind::S_compoundNonObjectExpression:       // compoundNonObjectExpression
        case symbol_kind::S_exprFixedTwoArg:                   // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                 // exprFixedThreeArg
        case symbol_kind::S_arrayManipulation:                 // arrayManipulation
        case symbol_kind::S_slice:                             // slice
        case symbol_kind::S_expressionArray:                   // expressionArray
        case symbol_kind::S_expressionObject:                  // expressionObject
        case symbol_kind::S_expressionFields:                  // expressionFields
        case symbol_kind::S_maths:                             // maths
        case symbol_kind::S_meta:                              // meta
        case symbol_kind::S_add:                               // add
        case symbol_kind::S_boolExprs:                         // boolExprs
        case symbol_kind::S_and:                               // and
        case symbol_kind::S_or:                                // or
        case symbol_kind::S_not:                               // not
        case symbol_kind::S_literalEscapes:                    // literalEscapes
        case symbol_kind::S_const:                             // const
        case symbol_kind::S_literal:                           // literal
        case symbol_kind::S_stringExps:                        // stringExps
        case symbol_kind::S_concat:                            // concat
        case symbol_kind::S_dateFromString:                    // dateFromString
        case symbol_kind::S_dateToString:                      // dateToString
        case symbol_kind::S_indexOfBytes:                      // indexOfBytes
        case symbol_kind::S_indexOfCP:                         // indexOfCP
        case symbol_kind::S_ltrim:                             // ltrim
        case symbol_kind::S_regexFind:                         // regexFind
        case symbol_kind::S_regexFindAll:                      // regexFindAll
        case symbol_kind::S_regexMatch:                        // regexMatch
        case symbol_kind::S_regexArgs:                         // regexArgs
        case symbol_kind::S_replaceOne:                        // replaceOne
        case symbol_kind::S_replaceAll:                        // replaceAll
        case symbol_kind::S_rtrim:                             // rtrim
        case symbol_kind::S_split:                             // split
        case symbol_kind::S_strLenBytes:                       // strLenBytes
        case symbol_kind::S_strLenCP:                          // strLenCP
        case symbol_kind::S_strcasecmp:                        // strcasecmp
        case symbol_kind::S_substr:                            // substr
        case symbol_kind::S_substrBytes:                       // substrBytes
        case symbol_kind::S_substrCP:                          // substrCP
        case symbol_kind::S_toLower:                           // toLower
        case symbol_kind::S_toUpper:                           // toUpper
        case symbol_kind::S_trim:                              // trim
        case symbol_kind::S_compExprs:                         // compExprs
        case symbol_kind::S_cmp:                               // cmp
        case symbol_kind::S_eq:                                // eq
        case symbol_kind::S_gt:                                // gt
        case symbol_kind::S_gte:                               // gte
        case symbol_kind::S_lt:                                // lt
        case symbol_kind::S_lte:                               // lte
        case symbol_kind::S_ne:                                // ne
        case symbol_kind::S_dateExps:                          // dateExps
        case symbol_kind::S_dateFromParts:                     // dateFromParts
        case symbol_kind::S_dateToParts:                       // dateToParts
        case symbol_kind::S_dayOfMonth:                        // dayOfMonth
        case symbol_kind::S_dayOfWeek:                         // dayOfWeek
        case symbol_kind::S_dayOfYear:                         // dayOfYear
        case symbol_kind::S_hour:                              // hour
        case symbol_kind::S_isoDayOfWeek:                      // isoDayOfWeek
        case symbol_kind::S_isoWeek:                           // isoWeek
        case symbol_kind::S_isoWeekYear:                       // isoWeekYear
        case symbol_kind::S_millisecond:                       // millisecond
        case symbol_kind::S_minute:                            // minute
        case symbol_kind::S_month:                             // month
        case symbol_kind::S_second:                            // second
        case symbol_kind::S_week:                              // week
        case symbol_kind::S_year:                              // year
        case symbol_kind::S_typeExpression:                    // typeExpression
        case symbol_kind::S_convert:                           // convert
        case symbol_kind::S_toBool:                            // toBool
        case symbol_kind::S_toDate:                            // toDate
        case symbol_kind::S_toDecimal:                         // toDecimal
        case symbol_kind::S_toDouble:                          // toDouble
        case symbol_kind::S_toInt:                             // toInt
        case symbol_kind::S_toLong:                            // toLong
        case symbol_kind::S_toObjectId:                        // toObjectId
        case symbol_kind::S_toString:                          // toString
        case symbol_kind::S_type:                              // type
        case symbol_kind::S_abs:                               // abs
        case symbol_kind::S_ceil:                              // ceil
        case symbol_kind::S_divide:                            // divide
        case symbol_kind::S_exponent:                          // exponent
        case symbol_kind::S_floor:                             // floor
        case symbol_kind::S_ln:                                // ln
        case symbol_kind::S_log:                               // log
        case symbol_kind::S_logten:                            // logten
        case symbol_kind::S_mod:                               // mod
        case symbol_kind::S_multiply:                          // multiply
        case symbol_kind::S_pow:                               // pow
        case symbol_kind::S_round:                             // round
        case symbol_kind::S_sqrt:                              // sqrt
        case symbol_kind::S_subtract:                          // subtract
        case symbol_kind::S_trunc:                             // trunc
        case symbol_kind::S_setExpression:                     // setExpression
        case symbol_kind::S_allElementsTrue:                   // allElementsTrue
        case symbol_kind::S_anyElementTrue:                    // anyElementTrue
        case symbol_kind::S_setDifference:                     // setDifference
        case symbol_kind::S_setEquals:                         // setEquals
        case symbol_kind::S_setIntersection:                   // setIntersection
        case symbol_kind::S_setIsSubset:                       // setIsSubset
        case symbol_kind::S_setUnion:                          // setUnion
        case symbol_kind::S_trig:                              // trig
        case symbol_kind::S_sin:                               // sin
        case symbol_kind::S_cos:                               // cos
        case symbol_kind::S_tan:                               // tan
        case symbol_kind::S_sinh:                              // sinh
        case symbol_kind::S_cosh:                              // cosh
        case symbol_kind::S_tanh:                              // tanh
        case symbol_kind::S_asin:                              // asin
        case symbol_kind::S_acos:                              // acos
        case symbol_kind::S_atan:                              // atan
        case symbol_kind::S_asinh:                             // asinh
        case symbol_kind::S_acosh:                             // acosh
        case symbol_kind::S_atanh:                             // atanh
        case symbol_kind::S_atan2:                             // atan2
        case symbol_kind::S_degreesToRadians:                  // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                  // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:        // nonArrayCompoundExpression
        case symbol_kind::S_nonArrayNonObjCompoundExpression:  // nonArrayNonObjCompoundExpression
        case symbol_kind::S_expressionSingletonArray:          // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:               // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:          // nonArrayNonObjExpression
        case symbol_kind::S_match:                             // match
        case symbol_kind::S_predicates:                        // predicates
        case symbol_kind::S_compoundMatchExprs:                // compoundMatchExprs
        case symbol_kind::S_predValue:                         // predValue
        case symbol_kind::S_additionalExprs:                   // additionalExprs
        case symbol_kind::S_sortSpecs:                         // sortSpecs
        case symbol_kind::S_specList:                          // specList
        case symbol_kind::S_metaSort:                          // metaSort
        case symbol_kind::S_oneOrNegOne:                       // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                   // metaSortKeyword
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

        case symbol_kind::S_projectField:           // projectField
        case symbol_kind::S_projectionObjectField:  // projectionObjectField
        case symbol_kind::S_expressionField:        // expressionField
        case symbol_kind::S_valueField:             // valueField
        case symbol_kind::S_onErrorArg:             // onErrorArg
        case symbol_kind::S_onNullArg:              // onNullArg
        case symbol_kind::S_formatArg:              // formatArg
        case symbol_kind::S_timezoneArg:            // timezoneArg
        case symbol_kind::S_charsArg:               // charsArg
        case symbol_kind::S_optionsArg:             // optionsArg
        case symbol_kind::S_hourArg:                // hourArg
        case symbol_kind::S_minuteArg:              // minuteArg
        case symbol_kind::S_secondArg:              // secondArg
        case symbol_kind::S_millisecondArg:         // millisecondArg
        case symbol_kind::S_dayArg:                 // dayArg
        case symbol_kind::S_isoWeekArg:             // isoWeekArg
        case symbol_kind::S_iso8601Arg:             // iso8601Arg
        case symbol_kind::S_monthArg:               // monthArg
        case symbol_kind::S_isoDayOfWeekArg:        // isoDayOfWeekArg
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
        case symbol_kind::S_existsExpr:             // existsExpr
        case symbol_kind::S_typeExpr:               // typeExpr
        case symbol_kind::S_commentExpr:            // commentExpr
        case symbol_kind::S_sortSpec:               // sortSpec
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

        case symbol_kind::S_dbPointer:                         // dbPointer
        case symbol_kind::S_javascript:                        // javascript
        case symbol_kind::S_symbol:                            // symbol
        case symbol_kind::S_javascriptWScope:                  // javascriptWScope
        case symbol_kind::S_int:                               // int
        case symbol_kind::S_timestamp:                         // timestamp
        case symbol_kind::S_long:                              // long
        case symbol_kind::S_double:                            // double
        case symbol_kind::S_decimal:                           // decimal
        case symbol_kind::S_minKey:                            // minKey
        case symbol_kind::S_maxKey:                            // maxKey
        case symbol_kind::S_value:                             // value
        case symbol_kind::S_string:                            // string
        case symbol_kind::S_aggregationFieldPath:              // aggregationFieldPath
        case symbol_kind::S_binary:                            // binary
        case symbol_kind::S_undefined:                         // undefined
        case symbol_kind::S_objectId:                          // objectId
        case symbol_kind::S_bool:                              // bool
        case symbol_kind::S_date:                              // date
        case symbol_kind::S_null:                              // null
        case symbol_kind::S_regex:                             // regex
        case symbol_kind::S_simpleValue:                       // simpleValue
        case symbol_kind::S_compoundValue:                     // compoundValue
        case symbol_kind::S_valueArray:                        // valueArray
        case symbol_kind::S_valueObject:                       // valueObject
        case symbol_kind::S_valueFields:                       // valueFields
        case symbol_kind::S_variable:                          // variable
        case symbol_kind::S_typeArray:                         // typeArray
        case symbol_kind::S_typeValue:                         // typeValue
        case symbol_kind::S_pipeline:                          // pipeline
        case symbol_kind::S_stageList:                         // stageList
        case symbol_kind::S_stage:                             // stage
        case symbol_kind::S_inhibitOptimization:               // inhibitOptimization
        case symbol_kind::S_unionWith:                         // unionWith
        case symbol_kind::S_skip:                              // skip
        case symbol_kind::S_limit:                             // limit
        case symbol_kind::S_project:                           // project
        case symbol_kind::S_sample:                            // sample
        case symbol_kind::S_projectFields:                     // projectFields
        case symbol_kind::S_projectionObjectFields:            // projectionObjectFields
        case symbol_kind::S_topLevelProjection:                // topLevelProjection
        case symbol_kind::S_projection:                        // projection
        case symbol_kind::S_projectionObject:                  // projectionObject
        case symbol_kind::S_num:                               // num
        case symbol_kind::S_expression:                        // expression
        case symbol_kind::S_compoundNonObjectExpression:       // compoundNonObjectExpression
        case symbol_kind::S_exprFixedTwoArg:                   // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                 // exprFixedThreeArg
        case symbol_kind::S_arrayManipulation:                 // arrayManipulation
        case symbol_kind::S_slice:                             // slice
        case symbol_kind::S_expressionArray:                   // expressionArray
        case symbol_kind::S_expressionObject:                  // expressionObject
        case symbol_kind::S_expressionFields:                  // expressionFields
        case symbol_kind::S_maths:                             // maths
        case symbol_kind::S_meta:                              // meta
        case symbol_kind::S_add:                               // add
        case symbol_kind::S_boolExprs:                         // boolExprs
        case symbol_kind::S_and:                               // and
        case symbol_kind::S_or:                                // or
        case symbol_kind::S_not:                               // not
        case symbol_kind::S_literalEscapes:                    // literalEscapes
        case symbol_kind::S_const:                             // const
        case symbol_kind::S_literal:                           // literal
        case symbol_kind::S_stringExps:                        // stringExps
        case symbol_kind::S_concat:                            // concat
        case symbol_kind::S_dateFromString:                    // dateFromString
        case symbol_kind::S_dateToString:                      // dateToString
        case symbol_kind::S_indexOfBytes:                      // indexOfBytes
        case symbol_kind::S_indexOfCP:                         // indexOfCP
        case symbol_kind::S_ltrim:                             // ltrim
        case symbol_kind::S_regexFind:                         // regexFind
        case symbol_kind::S_regexFindAll:                      // regexFindAll
        case symbol_kind::S_regexMatch:                        // regexMatch
        case symbol_kind::S_regexArgs:                         // regexArgs
        case symbol_kind::S_replaceOne:                        // replaceOne
        case symbol_kind::S_replaceAll:                        // replaceAll
        case symbol_kind::S_rtrim:                             // rtrim
        case symbol_kind::S_split:                             // split
        case symbol_kind::S_strLenBytes:                       // strLenBytes
        case symbol_kind::S_strLenCP:                          // strLenCP
        case symbol_kind::S_strcasecmp:                        // strcasecmp
        case symbol_kind::S_substr:                            // substr
        case symbol_kind::S_substrBytes:                       // substrBytes
        case symbol_kind::S_substrCP:                          // substrCP
        case symbol_kind::S_toLower:                           // toLower
        case symbol_kind::S_toUpper:                           // toUpper
        case symbol_kind::S_trim:                              // trim
        case symbol_kind::S_compExprs:                         // compExprs
        case symbol_kind::S_cmp:                               // cmp
        case symbol_kind::S_eq:                                // eq
        case symbol_kind::S_gt:                                // gt
        case symbol_kind::S_gte:                               // gte
        case symbol_kind::S_lt:                                // lt
        case symbol_kind::S_lte:                               // lte
        case symbol_kind::S_ne:                                // ne
        case symbol_kind::S_dateExps:                          // dateExps
        case symbol_kind::S_dateFromParts:                     // dateFromParts
        case symbol_kind::S_dateToParts:                       // dateToParts
        case symbol_kind::S_dayOfMonth:                        // dayOfMonth
        case symbol_kind::S_dayOfWeek:                         // dayOfWeek
        case symbol_kind::S_dayOfYear:                         // dayOfYear
        case symbol_kind::S_hour:                              // hour
        case symbol_kind::S_isoDayOfWeek:                      // isoDayOfWeek
        case symbol_kind::S_isoWeek:                           // isoWeek
        case symbol_kind::S_isoWeekYear:                       // isoWeekYear
        case symbol_kind::S_millisecond:                       // millisecond
        case symbol_kind::S_minute:                            // minute
        case symbol_kind::S_month:                             // month
        case symbol_kind::S_second:                            // second
        case symbol_kind::S_week:                              // week
        case symbol_kind::S_year:                              // year
        case symbol_kind::S_typeExpression:                    // typeExpression
        case symbol_kind::S_convert:                           // convert
        case symbol_kind::S_toBool:                            // toBool
        case symbol_kind::S_toDate:                            // toDate
        case symbol_kind::S_toDecimal:                         // toDecimal
        case symbol_kind::S_toDouble:                          // toDouble
        case symbol_kind::S_toInt:                             // toInt
        case symbol_kind::S_toLong:                            // toLong
        case symbol_kind::S_toObjectId:                        // toObjectId
        case symbol_kind::S_toString:                          // toString
        case symbol_kind::S_type:                              // type
        case symbol_kind::S_abs:                               // abs
        case symbol_kind::S_ceil:                              // ceil
        case symbol_kind::S_divide:                            // divide
        case symbol_kind::S_exponent:                          // exponent
        case symbol_kind::S_floor:                             // floor
        case symbol_kind::S_ln:                                // ln
        case symbol_kind::S_log:                               // log
        case symbol_kind::S_logten:                            // logten
        case symbol_kind::S_mod:                               // mod
        case symbol_kind::S_multiply:                          // multiply
        case symbol_kind::S_pow:                               // pow
        case symbol_kind::S_round:                             // round
        case symbol_kind::S_sqrt:                              // sqrt
        case symbol_kind::S_subtract:                          // subtract
        case symbol_kind::S_trunc:                             // trunc
        case symbol_kind::S_setExpression:                     // setExpression
        case symbol_kind::S_allElementsTrue:                   // allElementsTrue
        case symbol_kind::S_anyElementTrue:                    // anyElementTrue
        case symbol_kind::S_setDifference:                     // setDifference
        case symbol_kind::S_setEquals:                         // setEquals
        case symbol_kind::S_setIntersection:                   // setIntersection
        case symbol_kind::S_setIsSubset:                       // setIsSubset
        case symbol_kind::S_setUnion:                          // setUnion
        case symbol_kind::S_trig:                              // trig
        case symbol_kind::S_sin:                               // sin
        case symbol_kind::S_cos:                               // cos
        case symbol_kind::S_tan:                               // tan
        case symbol_kind::S_sinh:                              // sinh
        case symbol_kind::S_cosh:                              // cosh
        case symbol_kind::S_tanh:                              // tanh
        case symbol_kind::S_asin:                              // asin
        case symbol_kind::S_acos:                              // acos
        case symbol_kind::S_atan:                              // atan
        case symbol_kind::S_asinh:                             // asinh
        case symbol_kind::S_acosh:                             // acosh
        case symbol_kind::S_atanh:                             // atanh
        case symbol_kind::S_atan2:                             // atan2
        case symbol_kind::S_degreesToRadians:                  // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                  // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:        // nonArrayCompoundExpression
        case symbol_kind::S_nonArrayNonObjCompoundExpression:  // nonArrayNonObjCompoundExpression
        case symbol_kind::S_expressionSingletonArray:          // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:               // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:          // nonArrayNonObjExpression
        case symbol_kind::S_match:                             // match
        case symbol_kind::S_predicates:                        // predicates
        case symbol_kind::S_compoundMatchExprs:                // compoundMatchExprs
        case symbol_kind::S_predValue:                         // predValue
        case symbol_kind::S_additionalExprs:                   // additionalExprs
        case symbol_kind::S_sortSpecs:                         // sortSpecs
        case symbol_kind::S_specList:                          // specList
        case symbol_kind::S_metaSort:                          // metaSort
        case symbol_kind::S_oneOrNegOne:                       // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                   // metaSortKeyword
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

        case symbol_kind::S_projectField:           // projectField
        case symbol_kind::S_projectionObjectField:  // projectionObjectField
        case symbol_kind::S_expressionField:        // expressionField
        case symbol_kind::S_valueField:             // valueField
        case symbol_kind::S_onErrorArg:             // onErrorArg
        case symbol_kind::S_onNullArg:              // onNullArg
        case symbol_kind::S_formatArg:              // formatArg
        case symbol_kind::S_timezoneArg:            // timezoneArg
        case symbol_kind::S_charsArg:               // charsArg
        case symbol_kind::S_optionsArg:             // optionsArg
        case symbol_kind::S_hourArg:                // hourArg
        case symbol_kind::S_minuteArg:              // minuteArg
        case symbol_kind::S_secondArg:              // secondArg
        case symbol_kind::S_millisecondArg:         // millisecondArg
        case symbol_kind::S_dayArg:                 // dayArg
        case symbol_kind::S_isoWeekArg:             // isoWeekArg
        case symbol_kind::S_iso8601Arg:             // iso8601Arg
        case symbol_kind::S_monthArg:               // monthArg
        case symbol_kind::S_isoDayOfWeekArg:        // isoDayOfWeekArg
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
        case symbol_kind::S_existsExpr:             // existsExpr
        case symbol_kind::S_typeExpr:               // typeExpr
        case symbol_kind::S_commentExpr:            // commentExpr
        case symbol_kind::S_sortSpec:               // sortSpec
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

        case symbol_kind::S_dbPointer:                         // dbPointer
        case symbol_kind::S_javascript:                        // javascript
        case symbol_kind::S_symbol:                            // symbol
        case symbol_kind::S_javascriptWScope:                  // javascriptWScope
        case symbol_kind::S_int:                               // int
        case symbol_kind::S_timestamp:                         // timestamp
        case symbol_kind::S_long:                              // long
        case symbol_kind::S_double:                            // double
        case symbol_kind::S_decimal:                           // decimal
        case symbol_kind::S_minKey:                            // minKey
        case symbol_kind::S_maxKey:                            // maxKey
        case symbol_kind::S_value:                             // value
        case symbol_kind::S_string:                            // string
        case symbol_kind::S_aggregationFieldPath:              // aggregationFieldPath
        case symbol_kind::S_binary:                            // binary
        case symbol_kind::S_undefined:                         // undefined
        case symbol_kind::S_objectId:                          // objectId
        case symbol_kind::S_bool:                              // bool
        case symbol_kind::S_date:                              // date
        case symbol_kind::S_null:                              // null
        case symbol_kind::S_regex:                             // regex
        case symbol_kind::S_simpleValue:                       // simpleValue
        case symbol_kind::S_compoundValue:                     // compoundValue
        case symbol_kind::S_valueArray:                        // valueArray
        case symbol_kind::S_valueObject:                       // valueObject
        case symbol_kind::S_valueFields:                       // valueFields
        case symbol_kind::S_variable:                          // variable
        case symbol_kind::S_typeArray:                         // typeArray
        case symbol_kind::S_typeValue:                         // typeValue
        case symbol_kind::S_pipeline:                          // pipeline
        case symbol_kind::S_stageList:                         // stageList
        case symbol_kind::S_stage:                             // stage
        case symbol_kind::S_inhibitOptimization:               // inhibitOptimization
        case symbol_kind::S_unionWith:                         // unionWith
        case symbol_kind::S_skip:                              // skip
        case symbol_kind::S_limit:                             // limit
        case symbol_kind::S_project:                           // project
        case symbol_kind::S_sample:                            // sample
        case symbol_kind::S_projectFields:                     // projectFields
        case symbol_kind::S_projectionObjectFields:            // projectionObjectFields
        case symbol_kind::S_topLevelProjection:                // topLevelProjection
        case symbol_kind::S_projection:                        // projection
        case symbol_kind::S_projectionObject:                  // projectionObject
        case symbol_kind::S_num:                               // num
        case symbol_kind::S_expression:                        // expression
        case symbol_kind::S_compoundNonObjectExpression:       // compoundNonObjectExpression
        case symbol_kind::S_exprFixedTwoArg:                   // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                 // exprFixedThreeArg
        case symbol_kind::S_arrayManipulation:                 // arrayManipulation
        case symbol_kind::S_slice:                             // slice
        case symbol_kind::S_expressionArray:                   // expressionArray
        case symbol_kind::S_expressionObject:                  // expressionObject
        case symbol_kind::S_expressionFields:                  // expressionFields
        case symbol_kind::S_maths:                             // maths
        case symbol_kind::S_meta:                              // meta
        case symbol_kind::S_add:                               // add
        case symbol_kind::S_boolExprs:                         // boolExprs
        case symbol_kind::S_and:                               // and
        case symbol_kind::S_or:                                // or
        case symbol_kind::S_not:                               // not
        case symbol_kind::S_literalEscapes:                    // literalEscapes
        case symbol_kind::S_const:                             // const
        case symbol_kind::S_literal:                           // literal
        case symbol_kind::S_stringExps:                        // stringExps
        case symbol_kind::S_concat:                            // concat
        case symbol_kind::S_dateFromString:                    // dateFromString
        case symbol_kind::S_dateToString:                      // dateToString
        case symbol_kind::S_indexOfBytes:                      // indexOfBytes
        case symbol_kind::S_indexOfCP:                         // indexOfCP
        case symbol_kind::S_ltrim:                             // ltrim
        case symbol_kind::S_regexFind:                         // regexFind
        case symbol_kind::S_regexFindAll:                      // regexFindAll
        case symbol_kind::S_regexMatch:                        // regexMatch
        case symbol_kind::S_regexArgs:                         // regexArgs
        case symbol_kind::S_replaceOne:                        // replaceOne
        case symbol_kind::S_replaceAll:                        // replaceAll
        case symbol_kind::S_rtrim:                             // rtrim
        case symbol_kind::S_split:                             // split
        case symbol_kind::S_strLenBytes:                       // strLenBytes
        case symbol_kind::S_strLenCP:                          // strLenCP
        case symbol_kind::S_strcasecmp:                        // strcasecmp
        case symbol_kind::S_substr:                            // substr
        case symbol_kind::S_substrBytes:                       // substrBytes
        case symbol_kind::S_substrCP:                          // substrCP
        case symbol_kind::S_toLower:                           // toLower
        case symbol_kind::S_toUpper:                           // toUpper
        case symbol_kind::S_trim:                              // trim
        case symbol_kind::S_compExprs:                         // compExprs
        case symbol_kind::S_cmp:                               // cmp
        case symbol_kind::S_eq:                                // eq
        case symbol_kind::S_gt:                                // gt
        case symbol_kind::S_gte:                               // gte
        case symbol_kind::S_lt:                                // lt
        case symbol_kind::S_lte:                               // lte
        case symbol_kind::S_ne:                                // ne
        case symbol_kind::S_dateExps:                          // dateExps
        case symbol_kind::S_dateFromParts:                     // dateFromParts
        case symbol_kind::S_dateToParts:                       // dateToParts
        case symbol_kind::S_dayOfMonth:                        // dayOfMonth
        case symbol_kind::S_dayOfWeek:                         // dayOfWeek
        case symbol_kind::S_dayOfYear:                         // dayOfYear
        case symbol_kind::S_hour:                              // hour
        case symbol_kind::S_isoDayOfWeek:                      // isoDayOfWeek
        case symbol_kind::S_isoWeek:                           // isoWeek
        case symbol_kind::S_isoWeekYear:                       // isoWeekYear
        case symbol_kind::S_millisecond:                       // millisecond
        case symbol_kind::S_minute:                            // minute
        case symbol_kind::S_month:                             // month
        case symbol_kind::S_second:                            // second
        case symbol_kind::S_week:                              // week
        case symbol_kind::S_year:                              // year
        case symbol_kind::S_typeExpression:                    // typeExpression
        case symbol_kind::S_convert:                           // convert
        case symbol_kind::S_toBool:                            // toBool
        case symbol_kind::S_toDate:                            // toDate
        case symbol_kind::S_toDecimal:                         // toDecimal
        case symbol_kind::S_toDouble:                          // toDouble
        case symbol_kind::S_toInt:                             // toInt
        case symbol_kind::S_toLong:                            // toLong
        case symbol_kind::S_toObjectId:                        // toObjectId
        case symbol_kind::S_toString:                          // toString
        case symbol_kind::S_type:                              // type
        case symbol_kind::S_abs:                               // abs
        case symbol_kind::S_ceil:                              // ceil
        case symbol_kind::S_divide:                            // divide
        case symbol_kind::S_exponent:                          // exponent
        case symbol_kind::S_floor:                             // floor
        case symbol_kind::S_ln:                                // ln
        case symbol_kind::S_log:                               // log
        case symbol_kind::S_logten:                            // logten
        case symbol_kind::S_mod:                               // mod
        case symbol_kind::S_multiply:                          // multiply
        case symbol_kind::S_pow:                               // pow
        case symbol_kind::S_round:                             // round
        case symbol_kind::S_sqrt:                              // sqrt
        case symbol_kind::S_subtract:                          // subtract
        case symbol_kind::S_trunc:                             // trunc
        case symbol_kind::S_setExpression:                     // setExpression
        case symbol_kind::S_allElementsTrue:                   // allElementsTrue
        case symbol_kind::S_anyElementTrue:                    // anyElementTrue
        case symbol_kind::S_setDifference:                     // setDifference
        case symbol_kind::S_setEquals:                         // setEquals
        case symbol_kind::S_setIntersection:                   // setIntersection
        case symbol_kind::S_setIsSubset:                       // setIsSubset
        case symbol_kind::S_setUnion:                          // setUnion
        case symbol_kind::S_trig:                              // trig
        case symbol_kind::S_sin:                               // sin
        case symbol_kind::S_cos:                               // cos
        case symbol_kind::S_tan:                               // tan
        case symbol_kind::S_sinh:                              // sinh
        case symbol_kind::S_cosh:                              // cosh
        case symbol_kind::S_tanh:                              // tanh
        case symbol_kind::S_asin:                              // asin
        case symbol_kind::S_acos:                              // acos
        case symbol_kind::S_atan:                              // atan
        case symbol_kind::S_asinh:                             // asinh
        case symbol_kind::S_acosh:                             // acosh
        case symbol_kind::S_atanh:                             // atanh
        case symbol_kind::S_atan2:                             // atan2
        case symbol_kind::S_degreesToRadians:                  // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                  // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:        // nonArrayCompoundExpression
        case symbol_kind::S_nonArrayNonObjCompoundExpression:  // nonArrayNonObjCompoundExpression
        case symbol_kind::S_expressionSingletonArray:          // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:               // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:          // nonArrayNonObjExpression
        case symbol_kind::S_match:                             // match
        case symbol_kind::S_predicates:                        // predicates
        case symbol_kind::S_compoundMatchExprs:                // compoundMatchExprs
        case symbol_kind::S_predValue:                         // predValue
        case symbol_kind::S_additionalExprs:                   // additionalExprs
        case symbol_kind::S_sortSpecs:                         // sortSpecs
        case symbol_kind::S_specList:                          // specList
        case symbol_kind::S_metaSort:                          // metaSort
        case symbol_kind::S_oneOrNegOne:                       // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                   // metaSortKeyword
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

        case symbol_kind::S_projectField:           // projectField
        case symbol_kind::S_projectionObjectField:  // projectionObjectField
        case symbol_kind::S_expressionField:        // expressionField
        case symbol_kind::S_valueField:             // valueField
        case symbol_kind::S_onErrorArg:             // onErrorArg
        case symbol_kind::S_onNullArg:              // onNullArg
        case symbol_kind::S_formatArg:              // formatArg
        case symbol_kind::S_timezoneArg:            // timezoneArg
        case symbol_kind::S_charsArg:               // charsArg
        case symbol_kind::S_optionsArg:             // optionsArg
        case symbol_kind::S_hourArg:                // hourArg
        case symbol_kind::S_minuteArg:              // minuteArg
        case symbol_kind::S_secondArg:              // secondArg
        case symbol_kind::S_millisecondArg:         // millisecondArg
        case symbol_kind::S_dayArg:                 // dayArg
        case symbol_kind::S_isoWeekArg:             // isoWeekArg
        case symbol_kind::S_iso8601Arg:             // iso8601Arg
        case symbol_kind::S_monthArg:               // monthArg
        case symbol_kind::S_isoDayOfWeekArg:        // isoDayOfWeekArg
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
        case symbol_kind::S_existsExpr:             // existsExpr
        case symbol_kind::S_typeExpr:               // typeExpr
        case symbol_kind::S_commentExpr:            // commentExpr
        case symbol_kind::S_sortSpec:               // sortSpec
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

        case symbol_kind::S_dbPointer:                         // dbPointer
        case symbol_kind::S_javascript:                        // javascript
        case symbol_kind::S_symbol:                            // symbol
        case symbol_kind::S_javascriptWScope:                  // javascriptWScope
        case symbol_kind::S_int:                               // int
        case symbol_kind::S_timestamp:                         // timestamp
        case symbol_kind::S_long:                              // long
        case symbol_kind::S_double:                            // double
        case symbol_kind::S_decimal:                           // decimal
        case symbol_kind::S_minKey:                            // minKey
        case symbol_kind::S_maxKey:                            // maxKey
        case symbol_kind::S_value:                             // value
        case symbol_kind::S_string:                            // string
        case symbol_kind::S_aggregationFieldPath:              // aggregationFieldPath
        case symbol_kind::S_binary:                            // binary
        case symbol_kind::S_undefined:                         // undefined
        case symbol_kind::S_objectId:                          // objectId
        case symbol_kind::S_bool:                              // bool
        case symbol_kind::S_date:                              // date
        case symbol_kind::S_null:                              // null
        case symbol_kind::S_regex:                             // regex
        case symbol_kind::S_simpleValue:                       // simpleValue
        case symbol_kind::S_compoundValue:                     // compoundValue
        case symbol_kind::S_valueArray:                        // valueArray
        case symbol_kind::S_valueObject:                       // valueObject
        case symbol_kind::S_valueFields:                       // valueFields
        case symbol_kind::S_variable:                          // variable
        case symbol_kind::S_typeArray:                         // typeArray
        case symbol_kind::S_typeValue:                         // typeValue
        case symbol_kind::S_pipeline:                          // pipeline
        case symbol_kind::S_stageList:                         // stageList
        case symbol_kind::S_stage:                             // stage
        case symbol_kind::S_inhibitOptimization:               // inhibitOptimization
        case symbol_kind::S_unionWith:                         // unionWith
        case symbol_kind::S_skip:                              // skip
        case symbol_kind::S_limit:                             // limit
        case symbol_kind::S_project:                           // project
        case symbol_kind::S_sample:                            // sample
        case symbol_kind::S_projectFields:                     // projectFields
        case symbol_kind::S_projectionObjectFields:            // projectionObjectFields
        case symbol_kind::S_topLevelProjection:                // topLevelProjection
        case symbol_kind::S_projection:                        // projection
        case symbol_kind::S_projectionObject:                  // projectionObject
        case symbol_kind::S_num:                               // num
        case symbol_kind::S_expression:                        // expression
        case symbol_kind::S_compoundNonObjectExpression:       // compoundNonObjectExpression
        case symbol_kind::S_exprFixedTwoArg:                   // exprFixedTwoArg
        case symbol_kind::S_exprFixedThreeArg:                 // exprFixedThreeArg
        case symbol_kind::S_arrayManipulation:                 // arrayManipulation
        case symbol_kind::S_slice:                             // slice
        case symbol_kind::S_expressionArray:                   // expressionArray
        case symbol_kind::S_expressionObject:                  // expressionObject
        case symbol_kind::S_expressionFields:                  // expressionFields
        case symbol_kind::S_maths:                             // maths
        case symbol_kind::S_meta:                              // meta
        case symbol_kind::S_add:                               // add
        case symbol_kind::S_boolExprs:                         // boolExprs
        case symbol_kind::S_and:                               // and
        case symbol_kind::S_or:                                // or
        case symbol_kind::S_not:                               // not
        case symbol_kind::S_literalEscapes:                    // literalEscapes
        case symbol_kind::S_const:                             // const
        case symbol_kind::S_literal:                           // literal
        case symbol_kind::S_stringExps:                        // stringExps
        case symbol_kind::S_concat:                            // concat
        case symbol_kind::S_dateFromString:                    // dateFromString
        case symbol_kind::S_dateToString:                      // dateToString
        case symbol_kind::S_indexOfBytes:                      // indexOfBytes
        case symbol_kind::S_indexOfCP:                         // indexOfCP
        case symbol_kind::S_ltrim:                             // ltrim
        case symbol_kind::S_regexFind:                         // regexFind
        case symbol_kind::S_regexFindAll:                      // regexFindAll
        case symbol_kind::S_regexMatch:                        // regexMatch
        case symbol_kind::S_regexArgs:                         // regexArgs
        case symbol_kind::S_replaceOne:                        // replaceOne
        case symbol_kind::S_replaceAll:                        // replaceAll
        case symbol_kind::S_rtrim:                             // rtrim
        case symbol_kind::S_split:                             // split
        case symbol_kind::S_strLenBytes:                       // strLenBytes
        case symbol_kind::S_strLenCP:                          // strLenCP
        case symbol_kind::S_strcasecmp:                        // strcasecmp
        case symbol_kind::S_substr:                            // substr
        case symbol_kind::S_substrBytes:                       // substrBytes
        case symbol_kind::S_substrCP:                          // substrCP
        case symbol_kind::S_toLower:                           // toLower
        case symbol_kind::S_toUpper:                           // toUpper
        case symbol_kind::S_trim:                              // trim
        case symbol_kind::S_compExprs:                         // compExprs
        case symbol_kind::S_cmp:                               // cmp
        case symbol_kind::S_eq:                                // eq
        case symbol_kind::S_gt:                                // gt
        case symbol_kind::S_gte:                               // gte
        case symbol_kind::S_lt:                                // lt
        case symbol_kind::S_lte:                               // lte
        case symbol_kind::S_ne:                                // ne
        case symbol_kind::S_dateExps:                          // dateExps
        case symbol_kind::S_dateFromParts:                     // dateFromParts
        case symbol_kind::S_dateToParts:                       // dateToParts
        case symbol_kind::S_dayOfMonth:                        // dayOfMonth
        case symbol_kind::S_dayOfWeek:                         // dayOfWeek
        case symbol_kind::S_dayOfYear:                         // dayOfYear
        case symbol_kind::S_hour:                              // hour
        case symbol_kind::S_isoDayOfWeek:                      // isoDayOfWeek
        case symbol_kind::S_isoWeek:                           // isoWeek
        case symbol_kind::S_isoWeekYear:                       // isoWeekYear
        case symbol_kind::S_millisecond:                       // millisecond
        case symbol_kind::S_minute:                            // minute
        case symbol_kind::S_month:                             // month
        case symbol_kind::S_second:                            // second
        case symbol_kind::S_week:                              // week
        case symbol_kind::S_year:                              // year
        case symbol_kind::S_typeExpression:                    // typeExpression
        case symbol_kind::S_convert:                           // convert
        case symbol_kind::S_toBool:                            // toBool
        case symbol_kind::S_toDate:                            // toDate
        case symbol_kind::S_toDecimal:                         // toDecimal
        case symbol_kind::S_toDouble:                          // toDouble
        case symbol_kind::S_toInt:                             // toInt
        case symbol_kind::S_toLong:                            // toLong
        case symbol_kind::S_toObjectId:                        // toObjectId
        case symbol_kind::S_toString:                          // toString
        case symbol_kind::S_type:                              // type
        case symbol_kind::S_abs:                               // abs
        case symbol_kind::S_ceil:                              // ceil
        case symbol_kind::S_divide:                            // divide
        case symbol_kind::S_exponent:                          // exponent
        case symbol_kind::S_floor:                             // floor
        case symbol_kind::S_ln:                                // ln
        case symbol_kind::S_log:                               // log
        case symbol_kind::S_logten:                            // logten
        case symbol_kind::S_mod:                               // mod
        case symbol_kind::S_multiply:                          // multiply
        case symbol_kind::S_pow:                               // pow
        case symbol_kind::S_round:                             // round
        case symbol_kind::S_sqrt:                              // sqrt
        case symbol_kind::S_subtract:                          // subtract
        case symbol_kind::S_trunc:                             // trunc
        case symbol_kind::S_setExpression:                     // setExpression
        case symbol_kind::S_allElementsTrue:                   // allElementsTrue
        case symbol_kind::S_anyElementTrue:                    // anyElementTrue
        case symbol_kind::S_setDifference:                     // setDifference
        case symbol_kind::S_setEquals:                         // setEquals
        case symbol_kind::S_setIntersection:                   // setIntersection
        case symbol_kind::S_setIsSubset:                       // setIsSubset
        case symbol_kind::S_setUnion:                          // setUnion
        case symbol_kind::S_trig:                              // trig
        case symbol_kind::S_sin:                               // sin
        case symbol_kind::S_cos:                               // cos
        case symbol_kind::S_tan:                               // tan
        case symbol_kind::S_sinh:                              // sinh
        case symbol_kind::S_cosh:                              // cosh
        case symbol_kind::S_tanh:                              // tanh
        case symbol_kind::S_asin:                              // asin
        case symbol_kind::S_acos:                              // acos
        case symbol_kind::S_atan:                              // atan
        case symbol_kind::S_asinh:                             // asinh
        case symbol_kind::S_acosh:                             // acosh
        case symbol_kind::S_atanh:                             // atanh
        case symbol_kind::S_atan2:                             // atan2
        case symbol_kind::S_degreesToRadians:                  // degreesToRadians
        case symbol_kind::S_radiansToDegrees:                  // radiansToDegrees
        case symbol_kind::S_nonArrayExpression:                // nonArrayExpression
        case symbol_kind::S_nonArrayCompoundExpression:        // nonArrayCompoundExpression
        case symbol_kind::S_nonArrayNonObjCompoundExpression:  // nonArrayNonObjCompoundExpression
        case symbol_kind::S_expressionSingletonArray:          // expressionSingletonArray
        case symbol_kind::S_singleArgExpression:               // singleArgExpression
        case symbol_kind::S_nonArrayNonObjExpression:          // nonArrayNonObjExpression
        case symbol_kind::S_match:                             // match
        case symbol_kind::S_predicates:                        // predicates
        case symbol_kind::S_compoundMatchExprs:                // compoundMatchExprs
        case symbol_kind::S_predValue:                         // predValue
        case symbol_kind::S_additionalExprs:                   // additionalExprs
        case symbol_kind::S_sortSpecs:                         // sortSpecs
        case symbol_kind::S_specList:                          // specList
        case symbol_kind::S_metaSort:                          // metaSort
        case symbol_kind::S_oneOrNegOne:                       // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:                   // metaSortKeyword
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

        case symbol_kind::S_projectField:           // projectField
        case symbol_kind::S_projectionObjectField:  // projectionObjectField
        case symbol_kind::S_expressionField:        // expressionField
        case symbol_kind::S_valueField:             // valueField
        case symbol_kind::S_onErrorArg:             // onErrorArg
        case symbol_kind::S_onNullArg:              // onNullArg
        case symbol_kind::S_formatArg:              // formatArg
        case symbol_kind::S_timezoneArg:            // timezoneArg
        case symbol_kind::S_charsArg:               // charsArg
        case symbol_kind::S_optionsArg:             // optionsArg
        case symbol_kind::S_hourArg:                // hourArg
        case symbol_kind::S_minuteArg:              // minuteArg
        case symbol_kind::S_secondArg:              // secondArg
        case symbol_kind::S_millisecondArg:         // millisecondArg
        case symbol_kind::S_dayArg:                 // dayArg
        case symbol_kind::S_isoWeekArg:             // isoWeekArg
        case symbol_kind::S_iso8601Arg:             // iso8601Arg
        case symbol_kind::S_monthArg:               // monthArg
        case symbol_kind::S_isoDayOfWeekArg:        // isoDayOfWeekArg
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
        case symbol_kind::S_existsExpr:             // existsExpr
        case symbol_kind::S_typeExpr:               // typeExpr
        case symbol_kind::S_commentExpr:            // commentExpr
        case symbol_kind::S_sortSpec:               // sortSpec
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

                case symbol_kind::S_dbPointer:                    // dbPointer
                case symbol_kind::S_javascript:                   // javascript
                case symbol_kind::S_symbol:                       // symbol
                case symbol_kind::S_javascriptWScope:             // javascriptWScope
                case symbol_kind::S_int:                          // int
                case symbol_kind::S_timestamp:                    // timestamp
                case symbol_kind::S_long:                         // long
                case symbol_kind::S_double:                       // double
                case symbol_kind::S_decimal:                      // decimal
                case symbol_kind::S_minKey:                       // minKey
                case symbol_kind::S_maxKey:                       // maxKey
                case symbol_kind::S_value:                        // value
                case symbol_kind::S_string:                       // string
                case symbol_kind::S_aggregationFieldPath:         // aggregationFieldPath
                case symbol_kind::S_binary:                       // binary
                case symbol_kind::S_undefined:                    // undefined
                case symbol_kind::S_objectId:                     // objectId
                case symbol_kind::S_bool:                         // bool
                case symbol_kind::S_date:                         // date
                case symbol_kind::S_null:                         // null
                case symbol_kind::S_regex:                        // regex
                case symbol_kind::S_simpleValue:                  // simpleValue
                case symbol_kind::S_compoundValue:                // compoundValue
                case symbol_kind::S_valueArray:                   // valueArray
                case symbol_kind::S_valueObject:                  // valueObject
                case symbol_kind::S_valueFields:                  // valueFields
                case symbol_kind::S_variable:                     // variable
                case symbol_kind::S_typeArray:                    // typeArray
                case symbol_kind::S_typeValue:                    // typeValue
                case symbol_kind::S_pipeline:                     // pipeline
                case symbol_kind::S_stageList:                    // stageList
                case symbol_kind::S_stage:                        // stage
                case symbol_kind::S_inhibitOptimization:          // inhibitOptimization
                case symbol_kind::S_unionWith:                    // unionWith
                case symbol_kind::S_skip:                         // skip
                case symbol_kind::S_limit:                        // limit
                case symbol_kind::S_project:                      // project
                case symbol_kind::S_sample:                       // sample
                case symbol_kind::S_projectFields:                // projectFields
                case symbol_kind::S_projectionObjectFields:       // projectionObjectFields
                case symbol_kind::S_topLevelProjection:           // topLevelProjection
                case symbol_kind::S_projection:                   // projection
                case symbol_kind::S_projectionObject:             // projectionObject
                case symbol_kind::S_num:                          // num
                case symbol_kind::S_expression:                   // expression
                case symbol_kind::S_compoundNonObjectExpression:  // compoundNonObjectExpression
                case symbol_kind::S_exprFixedTwoArg:              // exprFixedTwoArg
                case symbol_kind::S_exprFixedThreeArg:            // exprFixedThreeArg
                case symbol_kind::S_arrayManipulation:            // arrayManipulation
                case symbol_kind::S_slice:                        // slice
                case symbol_kind::S_expressionArray:              // expressionArray
                case symbol_kind::S_expressionObject:             // expressionObject
                case symbol_kind::S_expressionFields:             // expressionFields
                case symbol_kind::S_maths:                        // maths
                case symbol_kind::S_meta:                         // meta
                case symbol_kind::S_add:                          // add
                case symbol_kind::S_boolExprs:                    // boolExprs
                case symbol_kind::S_and:                          // and
                case symbol_kind::S_or:                           // or
                case symbol_kind::S_not:                          // not
                case symbol_kind::S_literalEscapes:               // literalEscapes
                case symbol_kind::S_const:                        // const
                case symbol_kind::S_literal:                      // literal
                case symbol_kind::S_stringExps:                   // stringExps
                case symbol_kind::S_concat:                       // concat
                case symbol_kind::S_dateFromString:               // dateFromString
                case symbol_kind::S_dateToString:                 // dateToString
                case symbol_kind::S_indexOfBytes:                 // indexOfBytes
                case symbol_kind::S_indexOfCP:                    // indexOfCP
                case symbol_kind::S_ltrim:                        // ltrim
                case symbol_kind::S_regexFind:                    // regexFind
                case symbol_kind::S_regexFindAll:                 // regexFindAll
                case symbol_kind::S_regexMatch:                   // regexMatch
                case symbol_kind::S_regexArgs:                    // regexArgs
                case symbol_kind::S_replaceOne:                   // replaceOne
                case symbol_kind::S_replaceAll:                   // replaceAll
                case symbol_kind::S_rtrim:                        // rtrim
                case symbol_kind::S_split:                        // split
                case symbol_kind::S_strLenBytes:                  // strLenBytes
                case symbol_kind::S_strLenCP:                     // strLenCP
                case symbol_kind::S_strcasecmp:                   // strcasecmp
                case symbol_kind::S_substr:                       // substr
                case symbol_kind::S_substrBytes:                  // substrBytes
                case symbol_kind::S_substrCP:                     // substrCP
                case symbol_kind::S_toLower:                      // toLower
                case symbol_kind::S_toUpper:                      // toUpper
                case symbol_kind::S_trim:                         // trim
                case symbol_kind::S_compExprs:                    // compExprs
                case symbol_kind::S_cmp:                          // cmp
                case symbol_kind::S_eq:                           // eq
                case symbol_kind::S_gt:                           // gt
                case symbol_kind::S_gte:                          // gte
                case symbol_kind::S_lt:                           // lt
                case symbol_kind::S_lte:                          // lte
                case symbol_kind::S_ne:                           // ne
                case symbol_kind::S_dateExps:                     // dateExps
                case symbol_kind::S_dateFromParts:                // dateFromParts
                case symbol_kind::S_dateToParts:                  // dateToParts
                case symbol_kind::S_dayOfMonth:                   // dayOfMonth
                case symbol_kind::S_dayOfWeek:                    // dayOfWeek
                case symbol_kind::S_dayOfYear:                    // dayOfYear
                case symbol_kind::S_hour:                         // hour
                case symbol_kind::S_isoDayOfWeek:                 // isoDayOfWeek
                case symbol_kind::S_isoWeek:                      // isoWeek
                case symbol_kind::S_isoWeekYear:                  // isoWeekYear
                case symbol_kind::S_millisecond:                  // millisecond
                case symbol_kind::S_minute:                       // minute
                case symbol_kind::S_month:                        // month
                case symbol_kind::S_second:                       // second
                case symbol_kind::S_week:                         // week
                case symbol_kind::S_year:                         // year
                case symbol_kind::S_typeExpression:               // typeExpression
                case symbol_kind::S_convert:                      // convert
                case symbol_kind::S_toBool:                       // toBool
                case symbol_kind::S_toDate:                       // toDate
                case symbol_kind::S_toDecimal:                    // toDecimal
                case symbol_kind::S_toDouble:                     // toDouble
                case symbol_kind::S_toInt:                        // toInt
                case symbol_kind::S_toLong:                       // toLong
                case symbol_kind::S_toObjectId:                   // toObjectId
                case symbol_kind::S_toString:                     // toString
                case symbol_kind::S_type:                         // type
                case symbol_kind::S_abs:                          // abs
                case symbol_kind::S_ceil:                         // ceil
                case symbol_kind::S_divide:                       // divide
                case symbol_kind::S_exponent:                     // exponent
                case symbol_kind::S_floor:                        // floor
                case symbol_kind::S_ln:                           // ln
                case symbol_kind::S_log:                          // log
                case symbol_kind::S_logten:                       // logten
                case symbol_kind::S_mod:                          // mod
                case symbol_kind::S_multiply:                     // multiply
                case symbol_kind::S_pow:                          // pow
                case symbol_kind::S_round:                        // round
                case symbol_kind::S_sqrt:                         // sqrt
                case symbol_kind::S_subtract:                     // subtract
                case symbol_kind::S_trunc:                        // trunc
                case symbol_kind::S_setExpression:                // setExpression
                case symbol_kind::S_allElementsTrue:              // allElementsTrue
                case symbol_kind::S_anyElementTrue:               // anyElementTrue
                case symbol_kind::S_setDifference:                // setDifference
                case symbol_kind::S_setEquals:                    // setEquals
                case symbol_kind::S_setIntersection:              // setIntersection
                case symbol_kind::S_setIsSubset:                  // setIsSubset
                case symbol_kind::S_setUnion:                     // setUnion
                case symbol_kind::S_trig:                         // trig
                case symbol_kind::S_sin:                          // sin
                case symbol_kind::S_cos:                          // cos
                case symbol_kind::S_tan:                          // tan
                case symbol_kind::S_sinh:                         // sinh
                case symbol_kind::S_cosh:                         // cosh
                case symbol_kind::S_tanh:                         // tanh
                case symbol_kind::S_asin:                         // asin
                case symbol_kind::S_acos:                         // acos
                case symbol_kind::S_atan:                         // atan
                case symbol_kind::S_asinh:                        // asinh
                case symbol_kind::S_acosh:                        // acosh
                case symbol_kind::S_atanh:                        // atanh
                case symbol_kind::S_atan2:                        // atan2
                case symbol_kind::S_degreesToRadians:             // degreesToRadians
                case symbol_kind::S_radiansToDegrees:             // radiansToDegrees
                case symbol_kind::S_nonArrayExpression:           // nonArrayExpression
                case symbol_kind::S_nonArrayCompoundExpression:   // nonArrayCompoundExpression
                case symbol_kind::
                    S_nonArrayNonObjCompoundExpression:        // nonArrayNonObjCompoundExpression
                case symbol_kind::S_expressionSingletonArray:  // expressionSingletonArray
                case symbol_kind::S_singleArgExpression:       // singleArgExpression
                case symbol_kind::S_nonArrayNonObjExpression:  // nonArrayNonObjExpression
                case symbol_kind::S_match:                     // match
                case symbol_kind::S_predicates:                // predicates
                case symbol_kind::S_compoundMatchExprs:        // compoundMatchExprs
                case symbol_kind::S_predValue:                 // predValue
                case symbol_kind::S_additionalExprs:           // additionalExprs
                case symbol_kind::S_sortSpecs:                 // sortSpecs
                case symbol_kind::S_specList:                  // specList
                case symbol_kind::S_metaSort:                  // metaSort
                case symbol_kind::S_oneOrNegOne:               // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:           // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case symbol_kind::
                    S_aggregationProjectionFieldname:         // aggregationProjectionFieldname
                case symbol_kind::S_projectionFieldname:      // projectionFieldname
                case symbol_kind::S_expressionFieldname:      // expressionFieldname
                case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
                case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
                case symbol_kind::S_argAsProjectionPath:      // argAsProjectionPath
                case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
                case symbol_kind::S_sortFieldname:            // sortFieldname
                case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
                case symbol_kind::S_idAsProjectionPath:       // idAsProjectionPath
                case symbol_kind::S_valueFieldname:           // valueFieldname
                case symbol_kind::S_predFieldname:            // predFieldname
                case symbol_kind::S_logicalExprField:         // logicalExprField
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

                case symbol_kind::S_projectField:           // projectField
                case symbol_kind::S_projectionObjectField:  // projectionObjectField
                case symbol_kind::S_expressionField:        // expressionField
                case symbol_kind::S_valueField:             // valueField
                case symbol_kind::S_onErrorArg:             // onErrorArg
                case symbol_kind::S_onNullArg:              // onNullArg
                case symbol_kind::S_formatArg:              // formatArg
                case symbol_kind::S_timezoneArg:            // timezoneArg
                case symbol_kind::S_charsArg:               // charsArg
                case symbol_kind::S_optionsArg:             // optionsArg
                case symbol_kind::S_hourArg:                // hourArg
                case symbol_kind::S_minuteArg:              // minuteArg
                case symbol_kind::S_secondArg:              // secondArg
                case symbol_kind::S_millisecondArg:         // millisecondArg
                case symbol_kind::S_dayArg:                 // dayArg
                case symbol_kind::S_isoWeekArg:             // isoWeekArg
                case symbol_kind::S_iso8601Arg:             // iso8601Arg
                case symbol_kind::S_monthArg:               // monthArg
                case symbol_kind::S_isoDayOfWeekArg:        // isoDayOfWeekArg
                case symbol_kind::S_predicate:              // predicate
                case symbol_kind::S_logicalExpr:            // logicalExpr
                case symbol_kind::S_operatorExpression:     // operatorExpression
                case symbol_kind::S_notExpr:                // notExpr
                case symbol_kind::S_existsExpr:             // existsExpr
                case symbol_kind::S_typeExpr:               // typeExpr
                case symbol_kind::S_commentExpr:            // commentExpr
                case symbol_kind::S_sortSpec:               // sortSpec
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
#line 385 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2138 "parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH match
#line 388 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2146 "parser_gen.cpp"
                    break;

                    case 4:  // start: START_SORT sortSpecs
#line 391 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2154 "parser_gen.cpp"
                    break;

                    case 5:  // pipeline: "array" stageList "end of array"
#line 398 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2162 "parser_gen.cpp"
                    break;

                    case 6:  // stageList: %empty
#line 404 "grammar.yy"
                    {
                    }
#line 2168 "parser_gen.cpp"
                    break;

                    case 7:  // stageList: "object" stage "end of object" stageList
#line 405 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2176 "parser_gen.cpp"
                    break;

                    case 8:  // START_ORDERED_OBJECT: "object"
#line 413 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2182 "parser_gen.cpp"
                    break;

                    case 9:  // stage: inhibitOptimization
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2188 "parser_gen.cpp"
                    break;

                    case 10:  // stage: unionWith
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2194 "parser_gen.cpp"
                    break;

                    case 11:  // stage: skip
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2200 "parser_gen.cpp"
                    break;

                    case 12:  // stage: limit
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2206 "parser_gen.cpp"
                    break;

                    case 13:  // stage: project
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2212 "parser_gen.cpp"
                    break;

                    case 14:  // stage: sample
#line 416 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2218 "parser_gen.cpp"
                    break;

                    case 15:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 419 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2230 "parser_gen.cpp"
                    break;

                    case 16:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 429 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2238 "parser_gen.cpp"
                    break;

                    case 17:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 435 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2251 "parser_gen.cpp"
                    break;

                    case 18:  // num: int
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2257 "parser_gen.cpp"
                    break;

                    case 19:  // num: long
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2263 "parser_gen.cpp"
                    break;

                    case 20:  // num: double
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2269 "parser_gen.cpp"
                    break;

                    case 21:  // num: decimal
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2275 "parser_gen.cpp"
                    break;

                    case 22:  // skip: STAGE_SKIP num
#line 449 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2283 "parser_gen.cpp"
                    break;

                    case 23:  // limit: STAGE_LIMIT num
#line 454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2291 "parser_gen.cpp"
                    break;

                    case 24:  // project: STAGE_PROJECT "object" projectFields "end of object"
#line 459 "grammar.yy"
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
#line 2312 "parser_gen.cpp"
                    break;

                    case 25:  // projectFields: %empty
#line 478 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2320 "parser_gen.cpp"
                    break;

                    case 26:  // projectFields: projectFields projectField
#line 481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2329 "parser_gen.cpp"
                    break;

                    case 27:  // projectField: ID topLevelProjection
#line 488 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2337 "parser_gen.cpp"
                    break;

                    case 28:  // projectField: aggregationProjectionFieldname topLevelProjection
#line 491 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2345 "parser_gen.cpp"
                    break;

                    case 29:  // topLevelProjection: projection
#line 497 "grammar.yy"
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
#line 2361 "parser_gen.cpp"
                    break;

                    case 30:  // projection: string
#line 511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2367 "parser_gen.cpp"
                    break;

                    case 31:  // projection: binary
#line 512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2373 "parser_gen.cpp"
                    break;

                    case 32:  // projection: undefined
#line 513 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2379 "parser_gen.cpp"
                    break;

                    case 33:  // projection: objectId
#line 514 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2385 "parser_gen.cpp"
                    break;

                    case 34:  // projection: date
#line 515 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2391 "parser_gen.cpp"
                    break;

                    case 35:  // projection: null
#line 516 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2397 "parser_gen.cpp"
                    break;

                    case 36:  // projection: regex
#line 517 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2403 "parser_gen.cpp"
                    break;

                    case 37:  // projection: dbPointer
#line 518 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2409 "parser_gen.cpp"
                    break;

                    case 38:  // projection: javascript
#line 519 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2415 "parser_gen.cpp"
                    break;

                    case 39:  // projection: symbol
#line 520 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2421 "parser_gen.cpp"
                    break;

                    case 40:  // projection: javascriptWScope
#line 521 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2427 "parser_gen.cpp"
                    break;

                    case 41:  // projection: "1 (int)"
#line 522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2435 "parser_gen.cpp"
                    break;

                    case 42:  // projection: "-1 (int)"
#line 525 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2443 "parser_gen.cpp"
                    break;

                    case 43:  // projection: "arbitrary integer"
#line 528 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2451 "parser_gen.cpp"
                    break;

                    case 44:  // projection: "zero (int)"
#line 531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2459 "parser_gen.cpp"
                    break;

                    case 45:  // projection: "1 (long)"
#line 534 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2467 "parser_gen.cpp"
                    break;

                    case 46:  // projection: "-1 (long)"
#line 537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2475 "parser_gen.cpp"
                    break;

                    case 47:  // projection: "arbitrary long"
#line 540 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2483 "parser_gen.cpp"
                    break;

                    case 48:  // projection: "zero (long)"
#line 543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2491 "parser_gen.cpp"
                    break;

                    case 49:  // projection: "1 (double)"
#line 546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2499 "parser_gen.cpp"
                    break;

                    case 50:  // projection: "-1 (double)"
#line 549 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2507 "parser_gen.cpp"
                    break;

                    case 51:  // projection: "arbitrary double"
#line 552 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2515 "parser_gen.cpp"
                    break;

                    case 52:  // projection: "zero (double)"
#line 555 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2523 "parser_gen.cpp"
                    break;

                    case 53:  // projection: "1 (decimal)"
#line 558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2531 "parser_gen.cpp"
                    break;

                    case 54:  // projection: "-1 (decimal)"
#line 561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2539 "parser_gen.cpp"
                    break;

                    case 55:  // projection: "arbitrary decimal"
#line 564 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2547 "parser_gen.cpp"
                    break;

                    case 56:  // projection: "zero (decimal)"
#line 567 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2555 "parser_gen.cpp"
                    break;

                    case 57:  // projection: "true"
#line 570 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2563 "parser_gen.cpp"
                    break;

                    case 58:  // projection: "false"
#line 573 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2571 "parser_gen.cpp"
                    break;

                    case 59:  // projection: timestamp
#line 576 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2577 "parser_gen.cpp"
                    break;

                    case 60:  // projection: minKey
#line 577 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2583 "parser_gen.cpp"
                    break;

                    case 61:  // projection: maxKey
#line 578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2589 "parser_gen.cpp"
                    break;

                    case 62:  // projection: projectionObject
#line 579 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2595 "parser_gen.cpp"
                    break;

                    case 63:  // projection: compoundNonObjectExpression
#line 580 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2601 "parser_gen.cpp"
                    break;

                    case 64:  // aggregationProjectionFieldname: projectionFieldname
#line 585 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2611 "parser_gen.cpp"
                    break;

                    case 65:  // projectionFieldname: "fieldname"
#line 593 "grammar.yy"
                    {
                        auto components =
                            makeVector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
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
#line 2629 "parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: argAsProjectionPath
#line 606 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2635 "parser_gen.cpp"
                    break;

                    case 67:  // projectionFieldname: "fieldname containing dotted path"
#line 607 "grammar.yy"
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
#line 2653 "parser_gen.cpp"
                    break;

                    case 68:  // projectionObject: "object" projectionObjectFields "end of object"
#line 624 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2661 "parser_gen.cpp"
                    break;

                    case 69:  // projectionObjectFields: projectionObjectField
#line 631 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2670 "parser_gen.cpp"
                    break;

                    case 70:  // projectionObjectFields: projectionObjectFields
                              // projectionObjectField
#line 635 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2679 "parser_gen.cpp"
                    break;

                    case 71:  // projectionObjectField: idAsProjectionPath projection
#line 643 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2687 "parser_gen.cpp"
                    break;

                    case 72:  // projectionObjectField: aggregationProjectionFieldname projection
#line 646 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2695 "parser_gen.cpp"
                    break;

                    case 73:  // match: "object" predicates "end of object"
#line 652 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2703 "parser_gen.cpp"
                    break;

                    case 74:  // predicates: %empty
#line 658 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2711 "parser_gen.cpp"
                    break;

                    case 75:  // predicates: predicates predicate
#line 661 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2720 "parser_gen.cpp"
                    break;

                    case 76:  // predicate: predFieldname predValue
#line 667 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2728 "parser_gen.cpp"
                    break;

                    case 77:  // predicate: logicalExpr
#line 670 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2734 "parser_gen.cpp"
                    break;

                    case 78:  // predicate: commentExpr
#line 671 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2740 "parser_gen.cpp"
                    break;

                    case 79:  // predValue: simpleValue
#line 678 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2746 "parser_gen.cpp"
                    break;

                    case 80:  // predValue: "object" compoundMatchExprs "end of object"
#line 679 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2754 "parser_gen.cpp"
                    break;

                    case 81:  // compoundMatchExprs: %empty
#line 685 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2762 "parser_gen.cpp"
                    break;

                    case 82:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 688 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2771 "parser_gen.cpp"
                    break;

                    case 83:  // operatorExpression: notExpr
#line 696 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2777 "parser_gen.cpp"
                    break;

                    case 84:  // operatorExpression: existsExpr
#line 696 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2783 "parser_gen.cpp"
                    break;

                    case 85:  // operatorExpression: typeExpr
#line 696 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2789 "parser_gen.cpp"
                    break;

                    case 86:  // existsExpr: EXISTS value
#line 700 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2797 "parser_gen.cpp"
                    break;

                    case 87:  // typeArray: "array" typeValues "end of array"
#line 706 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2805 "parser_gen.cpp"
                    break;

                    case 88:  // typeValues: %empty
#line 712 "grammar.yy"
                    {
                    }
#line 2811 "parser_gen.cpp"
                    break;

                    case 89:  // typeValues: typeValues typeValue
#line 713 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2820 "parser_gen.cpp"
                    break;

                    case 90:  // typeValue: num
#line 720 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2826 "parser_gen.cpp"
                    break;

                    case 91:  // typeValue: string
#line 720 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2832 "parser_gen.cpp"
                    break;

                    case 92:  // typeExpr: TYPE typeValue
#line 724 "grammar.yy"
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
#line 2846 "parser_gen.cpp"
                    break;

                    case 93:  // typeExpr: TYPE typeArray
#line 733 "grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 2858 "parser_gen.cpp"
                    break;

                    case 94:  // commentExpr: COMMENT value
#line 743 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2866 "parser_gen.cpp"
                    break;

                    case 95:  // notExpr: NOT regex
#line 749 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2874 "parser_gen.cpp"
                    break;

                    case 96:  // notExpr: NOT "object" compoundMatchExprs operatorExpression "end of
                              // object"
#line 754 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2885 "parser_gen.cpp"
                    break;

                    case 97:  // logicalExpr: logicalExprField "array" additionalExprs match "end of
                              // array"
#line 764 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2895 "parser_gen.cpp"
                    break;

                    case 98:  // logicalExprField: AND
#line 772 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2901 "parser_gen.cpp"
                    break;

                    case 99:  // logicalExprField: OR
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2907 "parser_gen.cpp"
                    break;

                    case 100:  // logicalExprField: NOR
#line 774 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2913 "parser_gen.cpp"
                    break;

                    case 101:  // additionalExprs: %empty
#line 777 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2921 "parser_gen.cpp"
                    break;

                    case 102:  // additionalExprs: additionalExprs match
#line 780 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2930 "parser_gen.cpp"
                    break;

                    case 103:  // predFieldname: idAsUserFieldname
#line 787 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2936 "parser_gen.cpp"
                    break;

                    case 104:  // predFieldname: argAsUserFieldname
#line 787 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2942 "parser_gen.cpp"
                    break;

                    case 105:  // predFieldname: invariableUserFieldname
#line 787 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2948 "parser_gen.cpp"
                    break;

                    case 106:  // invariableUserFieldname: "fieldname"
#line 790 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2956 "parser_gen.cpp"
                    break;

                    case 107:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 798 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2964 "parser_gen.cpp"
                    break;

                    case 108:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 801 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2972 "parser_gen.cpp"
                    break;

                    case 109:  // stageAsUserFieldname: STAGE_SKIP
#line 804 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2980 "parser_gen.cpp"
                    break;

                    case 110:  // stageAsUserFieldname: STAGE_LIMIT
#line 807 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2988 "parser_gen.cpp"
                    break;

                    case 111:  // stageAsUserFieldname: STAGE_PROJECT
#line 810 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2996 "parser_gen.cpp"
                    break;

                    case 112:  // stageAsUserFieldname: STAGE_SAMPLE
#line 813 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 3004 "parser_gen.cpp"
                    break;

                    case 113:  // argAsUserFieldname: arg
#line 819 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 3012 "parser_gen.cpp"
                    break;

                    case 114:  // argAsProjectionPath: arg
#line 825 "grammar.yy"
                    {
                        auto components =
                            makeVector<std::string>(YY_MOVE(yystack_[0].value.as<std::string>()));
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
#line 3030 "parser_gen.cpp"
                    break;

                    case 115:  // arg: "coll argument"
#line 844 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 3038 "parser_gen.cpp"
                    break;

                    case 116:  // arg: "pipeline argument"
#line 847 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 3046 "parser_gen.cpp"
                    break;

                    case 117:  // arg: "size argument"
#line 850 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 3054 "parser_gen.cpp"
                    break;

                    case 118:  // arg: "input argument"
#line 853 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 3062 "parser_gen.cpp"
                    break;

                    case 119:  // arg: "to argument"
#line 856 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 3070 "parser_gen.cpp"
                    break;

                    case 120:  // arg: "onError argument"
#line 859 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 3078 "parser_gen.cpp"
                    break;

                    case 121:  // arg: "onNull argument"
#line 862 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 3086 "parser_gen.cpp"
                    break;

                    case 122:  // arg: "dateString argument"
#line 865 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 3094 "parser_gen.cpp"
                    break;

                    case 123:  // arg: "format argument"
#line 868 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 3102 "parser_gen.cpp"
                    break;

                    case 124:  // arg: "timezone argument"
#line 871 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 3110 "parser_gen.cpp"
                    break;

                    case 125:  // arg: "date argument"
#line 874 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 3118 "parser_gen.cpp"
                    break;

                    case 126:  // arg: "chars argument"
#line 877 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 3126 "parser_gen.cpp"
                    break;

                    case 127:  // arg: "regex argument"
#line 880 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3134 "parser_gen.cpp"
                    break;

                    case 128:  // arg: "options argument"
#line 883 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3142 "parser_gen.cpp"
                    break;

                    case 129:  // arg: "find argument"
#line 886 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3150 "parser_gen.cpp"
                    break;

                    case 130:  // arg: "replacement argument"
#line 889 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3158 "parser_gen.cpp"
                    break;

                    case 131:  // arg: "hour argument"
#line 892 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"hour"};
                    }
#line 3166 "parser_gen.cpp"
                    break;

                    case 132:  // arg: "year argument"
#line 895 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"year"};
                    }
#line 3174 "parser_gen.cpp"
                    break;

                    case 133:  // arg: "minute argument"
#line 898 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"minute"};
                    }
#line 3182 "parser_gen.cpp"
                    break;

                    case 134:  // arg: "second argument"
#line 901 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"second"};
                    }
#line 3190 "parser_gen.cpp"
                    break;

                    case 135:  // arg: "millisecond argument"
#line 904 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"millisecond"};
                    }
#line 3198 "parser_gen.cpp"
                    break;

                    case 136:  // arg: "day argument"
#line 907 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"day"};
                    }
#line 3206 "parser_gen.cpp"
                    break;

                    case 137:  // arg: "ISO day of week argument"
#line 910 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoDayOfWeek"};
                    }
#line 3214 "parser_gen.cpp"
                    break;

                    case 138:  // arg: "ISO week argument"
#line 913 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeek"};
                    }
#line 3222 "parser_gen.cpp"
                    break;

                    case 139:  // arg: "ISO week year argument"
#line 916 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"isoWeekYear"};
                    }
#line 3230 "parser_gen.cpp"
                    break;

                    case 140:  // arg: "ISO 8601 argument"
#line 919 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"iso8601"};
                    }
#line 3238 "parser_gen.cpp"
                    break;

                    case 141:  // arg: "month argument"
#line 922 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = UserFieldname{"month"};
                    }
#line 3246 "parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: ADD
#line 930 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3254 "parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: ATAN2
#line 933 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3262 "parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: AND
#line 936 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3270 "parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: CONST_EXPR
#line 939 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3278 "parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: LITERAL
#line 942 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3286 "parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: OR
#line 945 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3294 "parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: NOT
#line 948 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3302 "parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: CMP
#line 951 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3310 "parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: EQ
#line 954 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3318 "parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: GT
#line 957 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3326 "parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: GTE
#line 960 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3334 "parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: LT
#line 963 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3342 "parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: LTE
#line 966 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3350 "parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: NE
#line 969 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3358 "parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: CONVERT
#line 972 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3366 "parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: TO_BOOL
#line 975 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3374 "parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: TO_DATE
#line 978 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3382 "parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: TO_DECIMAL
#line 981 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3390 "parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: TO_DOUBLE
#line 984 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3398 "parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: TO_INT
#line 987 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3406 "parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: TO_LONG
#line 990 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3414 "parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 993 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3422 "parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: TO_STRING
#line 996 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3430 "parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: TYPE
#line 999 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3438 "parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: ABS
#line 1002 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3446 "parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: CEIL
#line 1005 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3454 "parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: DIVIDE
#line 1008 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3462 "parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: EXPONENT
#line 1011 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3470 "parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: FLOOR
#line 1014 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3478 "parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: LN
#line 1017 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3486 "parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: LOG
#line 1020 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3494 "parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: LOGTEN
#line 1023 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3502 "parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: MOD
#line 1026 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3510 "parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: MULTIPLY
#line 1029 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3518 "parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: POW
#line 1032 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3526 "parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: ROUND
#line 1035 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3534 "parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: "slice"
#line 1038 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3542 "parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: SQRT
#line 1041 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3550 "parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: SUBTRACT
#line 1044 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3558 "parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: TRUNC
#line 1047 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3566 "parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: CONCAT
#line 1050 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3574 "parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: DATE_FROM_PARTS
#line 1053 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromParts"};
                    }
#line 3582 "parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: DATE_TO_PARTS
#line 1056 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToParts"};
                    }
#line 3590 "parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: DAY_OF_MONTH
#line 1059 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfMonth"};
                    }
#line 3598 "parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: DAY_OF_WEEK
#line 1062 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfWeek"};
                    }
#line 3606 "parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: DAY_OF_YEAR
#line 1065 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dayOfYear"};
                    }
#line 3614 "parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: HOUR
#line 1068 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$hour"};
                    }
#line 3622 "parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: ISO_DAY_OF_WEEK
#line 1071 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoDayOfWeek"};
                    }
#line 3630 "parser_gen.cpp"
                    break;

                    case 190:  // aggExprAsUserFieldname: ISO_WEEK
#line 1074 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeek"};
                    }
#line 3638 "parser_gen.cpp"
                    break;

                    case 191:  // aggExprAsUserFieldname: ISO_WEEK_YEAR
#line 1077 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$isoWeekYear"};
                    }
#line 3646 "parser_gen.cpp"
                    break;

                    case 192:  // aggExprAsUserFieldname: MILLISECOND
#line 1080 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$millisecond"};
                    }
#line 3654 "parser_gen.cpp"
                    break;

                    case 193:  // aggExprAsUserFieldname: MINUTE
#line 1083 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$minute"};
                    }
#line 3662 "parser_gen.cpp"
                    break;

                    case 194:  // aggExprAsUserFieldname: MONTH
#line 1086 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$month"};
                    }
#line 3670 "parser_gen.cpp"
                    break;

                    case 195:  // aggExprAsUserFieldname: SECOND
#line 1089 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$second"};
                    }
#line 3678 "parser_gen.cpp"
                    break;

                    case 196:  // aggExprAsUserFieldname: WEEK
#line 1092 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$week"};
                    }
#line 3686 "parser_gen.cpp"
                    break;

                    case 197:  // aggExprAsUserFieldname: YEAR
#line 1095 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$year"};
                    }
#line 3694 "parser_gen.cpp"
                    break;

                    case 198:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 1098 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3702 "parser_gen.cpp"
                    break;

                    case 199:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 1101 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3710 "parser_gen.cpp"
                    break;

                    case 200:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 1104 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3718 "parser_gen.cpp"
                    break;

                    case 201:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 1107 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3726 "parser_gen.cpp"
                    break;

                    case 202:  // aggExprAsUserFieldname: LTRIM
#line 1110 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3734 "parser_gen.cpp"
                    break;

                    case 203:  // aggExprAsUserFieldname: META
#line 1113 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3742 "parser_gen.cpp"
                    break;

                    case 204:  // aggExprAsUserFieldname: REGEX_FIND
#line 1116 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3750 "parser_gen.cpp"
                    break;

                    case 205:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3758 "parser_gen.cpp"
                    break;

                    case 206:  // aggExprAsUserFieldname: REGEX_MATCH
#line 1122 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3766 "parser_gen.cpp"
                    break;

                    case 207:  // aggExprAsUserFieldname: REPLACE_ONE
#line 1125 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3774 "parser_gen.cpp"
                    break;

                    case 208:  // aggExprAsUserFieldname: REPLACE_ALL
#line 1128 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3782 "parser_gen.cpp"
                    break;

                    case 209:  // aggExprAsUserFieldname: RTRIM
#line 1131 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3790 "parser_gen.cpp"
                    break;

                    case 210:  // aggExprAsUserFieldname: SPLIT
#line 1134 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3798 "parser_gen.cpp"
                    break;

                    case 211:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 1137 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3806 "parser_gen.cpp"
                    break;

                    case 212:  // aggExprAsUserFieldname: STR_LEN_CP
#line 1140 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3814 "parser_gen.cpp"
                    break;

                    case 213:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 1143 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3822 "parser_gen.cpp"
                    break;

                    case 214:  // aggExprAsUserFieldname: SUBSTR
#line 1146 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3830 "parser_gen.cpp"
                    break;

                    case 215:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 1149 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3838 "parser_gen.cpp"
                    break;

                    case 216:  // aggExprAsUserFieldname: SUBSTR_CP
#line 1152 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3846 "parser_gen.cpp"
                    break;

                    case 217:  // aggExprAsUserFieldname: TO_LOWER
#line 1155 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3854 "parser_gen.cpp"
                    break;

                    case 218:  // aggExprAsUserFieldname: TRIM
#line 1158 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3862 "parser_gen.cpp"
                    break;

                    case 219:  // aggExprAsUserFieldname: TO_UPPER
#line 1161 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3870 "parser_gen.cpp"
                    break;

                    case 220:  // aggExprAsUserFieldname: "allElementsTrue"
#line 1164 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3878 "parser_gen.cpp"
                    break;

                    case 221:  // aggExprAsUserFieldname: "anyElementTrue"
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3886 "parser_gen.cpp"
                    break;

                    case 222:  // aggExprAsUserFieldname: "setDifference"
#line 1170 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3894 "parser_gen.cpp"
                    break;

                    case 223:  // aggExprAsUserFieldname: "setEquals"
#line 1173 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3902 "parser_gen.cpp"
                    break;

                    case 224:  // aggExprAsUserFieldname: "setIntersection"
#line 1176 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3910 "parser_gen.cpp"
                    break;

                    case 225:  // aggExprAsUserFieldname: "setIsSubset"
#line 1179 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3918 "parser_gen.cpp"
                    break;

                    case 226:  // aggExprAsUserFieldname: "setUnion"
#line 1182 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3926 "parser_gen.cpp"
                    break;

                    case 227:  // aggExprAsUserFieldname: SIN
#line 1185 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 3934 "parser_gen.cpp"
                    break;

                    case 228:  // aggExprAsUserFieldname: COS
#line 1188 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 3942 "parser_gen.cpp"
                    break;

                    case 229:  // aggExprAsUserFieldname: TAN
#line 1191 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 3950 "parser_gen.cpp"
                    break;

                    case 230:  // aggExprAsUserFieldname: SINH
#line 1194 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 3958 "parser_gen.cpp"
                    break;

                    case 231:  // aggExprAsUserFieldname: COSH
#line 1197 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 3966 "parser_gen.cpp"
                    break;

                    case 232:  // aggExprAsUserFieldname: TANH
#line 1200 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 3974 "parser_gen.cpp"
                    break;

                    case 233:  // aggExprAsUserFieldname: ASIN
#line 1203 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 3982 "parser_gen.cpp"
                    break;

                    case 234:  // aggExprAsUserFieldname: ACOS
#line 1206 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 3990 "parser_gen.cpp"
                    break;

                    case 235:  // aggExprAsUserFieldname: ATAN
#line 1209 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 3998 "parser_gen.cpp"
                    break;

                    case 236:  // aggExprAsUserFieldname: ASINH
#line 1212 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 4006 "parser_gen.cpp"
                    break;

                    case 237:  // aggExprAsUserFieldname: ACOSH
#line 1215 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 4014 "parser_gen.cpp"
                    break;

                    case 238:  // aggExprAsUserFieldname: ATANH
#line 1218 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 4022 "parser_gen.cpp"
                    break;

                    case 239:  // aggExprAsUserFieldname: DEGREES_TO_RADIANS
#line 1221 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 4030 "parser_gen.cpp"
                    break;

                    case 240:  // aggExprAsUserFieldname: RADIANS_TO_DEGREES
#line 1224 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 4038 "parser_gen.cpp"
                    break;

                    case 241:  // string: "string"
#line 1231 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 4046 "parser_gen.cpp"
                    break;

                    case 242:  // string: "geoNearDistance"
#line 1236 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 4054 "parser_gen.cpp"
                    break;

                    case 243:  // string: "geoNearPoint"
#line 1239 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 4062 "parser_gen.cpp"
                    break;

                    case 244:  // string: "indexKey"
#line 1242 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 4070 "parser_gen.cpp"
                    break;

                    case 245:  // string: "randVal"
#line 1245 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 4078 "parser_gen.cpp"
                    break;

                    case 246:  // string: "recordId"
#line 1248 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 4086 "parser_gen.cpp"
                    break;

                    case 247:  // string: "searchHighlights"
#line 1251 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 4094 "parser_gen.cpp"
                    break;

                    case 248:  // string: "searchScore"
#line 1254 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 4102 "parser_gen.cpp"
                    break;

                    case 249:  // string: "sortKey"
#line 1257 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 4110 "parser_gen.cpp"
                    break;

                    case 250:  // string: "textScore"
#line 1260 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 4118 "parser_gen.cpp"
                    break;

                    case 251:  // aggregationFieldPath: "$-prefixed string"
#line 1266 "grammar.yy"
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
#line 4134 "parser_gen.cpp"
                    break;

                    case 252:  // variable: "$$-prefixed string"
#line 1280 "grammar.yy"
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
#line 4150 "parser_gen.cpp"
                    break;

                    case 253:  // binary: "BinData"
#line 1294 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 4158 "parser_gen.cpp"
                    break;

                    case 254:  // undefined: "undefined"
#line 1300 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 4166 "parser_gen.cpp"
                    break;

                    case 255:  // objectId: "ObjectID"
#line 1306 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 4174 "parser_gen.cpp"
                    break;

                    case 256:  // date: "Date"
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 4182 "parser_gen.cpp"
                    break;

                    case 257:  // null: "null"
#line 1318 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 4190 "parser_gen.cpp"
                    break;

                    case 258:  // regex: "regex"
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 4198 "parser_gen.cpp"
                    break;

                    case 259:  // dbPointer: "dbPointer"
#line 1330 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 4206 "parser_gen.cpp"
                    break;

                    case 260:  // javascript: "Code"
#line 1336 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 4214 "parser_gen.cpp"
                    break;

                    case 261:  // symbol: "Symbol"
#line 1342 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 4222 "parser_gen.cpp"
                    break;

                    case 262:  // javascriptWScope: "CodeWScope"
#line 1348 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 4230 "parser_gen.cpp"
                    break;

                    case 263:  // timestamp: "Timestamp"
#line 1354 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 4238 "parser_gen.cpp"
                    break;

                    case 264:  // minKey: "minKey"
#line 1360 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 4246 "parser_gen.cpp"
                    break;

                    case 265:  // maxKey: "maxKey"
#line 1366 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 4254 "parser_gen.cpp"
                    break;

                    case 266:  // int: "arbitrary integer"
#line 1372 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 4262 "parser_gen.cpp"
                    break;

                    case 267:  // int: "zero (int)"
#line 1375 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 4270 "parser_gen.cpp"
                    break;

                    case 268:  // int: "1 (int)"
#line 1378 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 4278 "parser_gen.cpp"
                    break;

                    case 269:  // int: "-1 (int)"
#line 1381 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 4286 "parser_gen.cpp"
                    break;

                    case 270:  // long: "arbitrary long"
#line 1387 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 4294 "parser_gen.cpp"
                    break;

                    case 271:  // long: "zero (long)"
#line 1390 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 4302 "parser_gen.cpp"
                    break;

                    case 272:  // long: "1 (long)"
#line 1393 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 4310 "parser_gen.cpp"
                    break;

                    case 273:  // long: "-1 (long)"
#line 1396 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 4318 "parser_gen.cpp"
                    break;

                    case 274:  // double: "arbitrary double"
#line 1402 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 4326 "parser_gen.cpp"
                    break;

                    case 275:  // double: "zero (double)"
#line 1405 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 4334 "parser_gen.cpp"
                    break;

                    case 276:  // double: "1 (double)"
#line 1408 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4342 "parser_gen.cpp"
                    break;

                    case 277:  // double: "-1 (double)"
#line 1411 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4350 "parser_gen.cpp"
                    break;

                    case 278:  // decimal: "arbitrary decimal"
#line 1417 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4358 "parser_gen.cpp"
                    break;

                    case 279:  // decimal: "zero (decimal)"
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4366 "parser_gen.cpp"
                    break;

                    case 280:  // decimal: "1 (decimal)"
#line 1423 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4374 "parser_gen.cpp"
                    break;

                    case 281:  // decimal: "-1 (decimal)"
#line 1426 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4382 "parser_gen.cpp"
                    break;

                    case 282:  // bool: "true"
#line 1432 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4390 "parser_gen.cpp"
                    break;

                    case 283:  // bool: "false"
#line 1435 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4398 "parser_gen.cpp"
                    break;

                    case 284:  // simpleValue: string
#line 1441 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4404 "parser_gen.cpp"
                    break;

                    case 285:  // simpleValue: aggregationFieldPath
#line 1442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4410 "parser_gen.cpp"
                    break;

                    case 286:  // simpleValue: variable
#line 1443 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4416 "parser_gen.cpp"
                    break;

                    case 287:  // simpleValue: binary
#line 1444 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4422 "parser_gen.cpp"
                    break;

                    case 288:  // simpleValue: undefined
#line 1445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4428 "parser_gen.cpp"
                    break;

                    case 289:  // simpleValue: objectId
#line 1446 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4434 "parser_gen.cpp"
                    break;

                    case 290:  // simpleValue: date
#line 1447 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4440 "parser_gen.cpp"
                    break;

                    case 291:  // simpleValue: null
#line 1448 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4446 "parser_gen.cpp"
                    break;

                    case 292:  // simpleValue: regex
#line 1449 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4452 "parser_gen.cpp"
                    break;

                    case 293:  // simpleValue: dbPointer
#line 1450 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4458 "parser_gen.cpp"
                    break;

                    case 294:  // simpleValue: javascript
#line 1451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4464 "parser_gen.cpp"
                    break;

                    case 295:  // simpleValue: symbol
#line 1452 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4470 "parser_gen.cpp"
                    break;

                    case 296:  // simpleValue: javascriptWScope
#line 1453 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4476 "parser_gen.cpp"
                    break;

                    case 297:  // simpleValue: int
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4482 "parser_gen.cpp"
                    break;

                    case 298:  // simpleValue: long
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4488 "parser_gen.cpp"
                    break;

                    case 299:  // simpleValue: double
#line 1456 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4494 "parser_gen.cpp"
                    break;

                    case 300:  // simpleValue: decimal
#line 1457 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4500 "parser_gen.cpp"
                    break;

                    case 301:  // simpleValue: bool
#line 1458 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4506 "parser_gen.cpp"
                    break;

                    case 302:  // simpleValue: timestamp
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4512 "parser_gen.cpp"
                    break;

                    case 303:  // simpleValue: minKey
#line 1460 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4518 "parser_gen.cpp"
                    break;

                    case 304:  // simpleValue: maxKey
#line 1461 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4524 "parser_gen.cpp"
                    break;

                    case 305:  // expressions: %empty
#line 1468 "grammar.yy"
                    {
                    }
#line 4530 "parser_gen.cpp"
                    break;

                    case 306:  // expressions: expressions expression
#line 1469 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4539 "parser_gen.cpp"
                    break;

                    case 307:  // expression: simpleValue
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4545 "parser_gen.cpp"
                    break;

                    case 308:  // expression: expressionObject
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4551 "parser_gen.cpp"
                    break;

                    case 309:  // expression: expressionArray
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4557 "parser_gen.cpp"
                    break;

                    case 310:  // expression: nonArrayNonObjCompoundExpression
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4563 "parser_gen.cpp"
                    break;

                    case 311:  // nonArrayExpression: simpleValue
#line 1480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4569 "parser_gen.cpp"
                    break;

                    case 312:  // nonArrayExpression: nonArrayCompoundExpression
#line 1480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4575 "parser_gen.cpp"
                    break;

                    case 313:  // nonArrayNonObjExpression: simpleValue
#line 1484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4581 "parser_gen.cpp"
                    break;

                    case 314:  // nonArrayNonObjExpression: nonArrayNonObjCompoundExpression
#line 1484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4587 "parser_gen.cpp"
                    break;

                    case 315:  // nonArrayCompoundExpression: expressionObject
#line 1488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4593 "parser_gen.cpp"
                    break;

                    case 316:  // nonArrayCompoundExpression: nonArrayNonObjCompoundExpression
#line 1488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4599 "parser_gen.cpp"
                    break;

                    case 317:  // nonArrayNonObjCompoundExpression: arrayManipulation
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4605 "parser_gen.cpp"
                    break;

                    case 318:  // nonArrayNonObjCompoundExpression: maths
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4611 "parser_gen.cpp"
                    break;

                    case 319:  // nonArrayNonObjCompoundExpression: meta
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4617 "parser_gen.cpp"
                    break;

                    case 320:  // nonArrayNonObjCompoundExpression: boolExprs
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4623 "parser_gen.cpp"
                    break;

                    case 321:  // nonArrayNonObjCompoundExpression: literalEscapes
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4629 "parser_gen.cpp"
                    break;

                    case 322:  // nonArrayNonObjCompoundExpression: compExprs
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4635 "parser_gen.cpp"
                    break;

                    case 323:  // nonArrayNonObjCompoundExpression: typeExpression
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4641 "parser_gen.cpp"
                    break;

                    case 324:  // nonArrayNonObjCompoundExpression: stringExps
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4647 "parser_gen.cpp"
                    break;

                    case 325:  // nonArrayNonObjCompoundExpression: setExpression
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4653 "parser_gen.cpp"
                    break;

                    case 326:  // nonArrayNonObjCompoundExpression: trig
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4659 "parser_gen.cpp"
                    break;

                    case 327:  // nonArrayNonObjCompoundExpression: dateExps
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4665 "parser_gen.cpp"
                    break;

                    case 328:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4673 "parser_gen.cpp"
                    break;

                    case 329:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1505 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4681 "parser_gen.cpp"
                    break;

                    case 330:  // compoundNonObjectExpression: expressionArray
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4687 "parser_gen.cpp"
                    break;

                    case 331:  // compoundNonObjectExpression: nonArrayNonObjCompoundExpression
#line 1511 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4693 "parser_gen.cpp"
                    break;

                    case 332:  // arrayManipulation: slice
#line 1514 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4699 "parser_gen.cpp"
                    break;

                    case 333:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1518 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4708 "parser_gen.cpp"
                    break;

                    case 334:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4717 "parser_gen.cpp"
                    break;

                    case 335:  // expressionArray: "array" expressions "end of array"
#line 1531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4725 "parser_gen.cpp"
                    break;

                    case 336:  // expressionSingletonArray: "array" expression "end of array"
#line 1538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4733 "parser_gen.cpp"
                    break;

                    case 337:  // singleArgExpression: nonArrayExpression
#line 1543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4739 "parser_gen.cpp"
                    break;

                    case 338:  // singleArgExpression: expressionSingletonArray
#line 1543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4745 "parser_gen.cpp"
                    break;

                    case 339:  // expressionObject: "object" expressionFields "end of object"
#line 1548 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4753 "parser_gen.cpp"
                    break;

                    case 340:  // expressionFields: %empty
#line 1554 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4761 "parser_gen.cpp"
                    break;

                    case 341:  // expressionFields: expressionFields expressionField
#line 1557 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4770 "parser_gen.cpp"
                    break;

                    case 342:  // expressionField: expressionFieldname expression
#line 1564 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4778 "parser_gen.cpp"
                    break;

                    case 343:  // expressionFieldname: invariableUserFieldname
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4784 "parser_gen.cpp"
                    break;

                    case 344:  // expressionFieldname: stageAsUserFieldname
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4790 "parser_gen.cpp"
                    break;

                    case 345:  // expressionFieldname: argAsUserFieldname
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4796 "parser_gen.cpp"
                    break;

                    case 346:  // expressionFieldname: idAsUserFieldname
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4802 "parser_gen.cpp"
                    break;

                    case 347:  // idAsUserFieldname: ID
#line 1575 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4810 "parser_gen.cpp"
                    break;

                    case 348:  // idAsProjectionPath: ID
#line 1581 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 4818 "parser_gen.cpp"
                    break;

                    case 349:  // maths: add
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4824 "parser_gen.cpp"
                    break;

                    case 350:  // maths: abs
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4830 "parser_gen.cpp"
                    break;

                    case 351:  // maths: ceil
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4836 "parser_gen.cpp"
                    break;

                    case 352:  // maths: divide
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4842 "parser_gen.cpp"
                    break;

                    case 353:  // maths: exponent
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4848 "parser_gen.cpp"
                    break;

                    case 354:  // maths: floor
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4854 "parser_gen.cpp"
                    break;

                    case 355:  // maths: ln
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4860 "parser_gen.cpp"
                    break;

                    case 356:  // maths: log
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4866 "parser_gen.cpp"
                    break;

                    case 357:  // maths: logten
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4872 "parser_gen.cpp"
                    break;

                    case 358:  // maths: mod
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4878 "parser_gen.cpp"
                    break;

                    case 359:  // maths: multiply
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4884 "parser_gen.cpp"
                    break;

                    case 360:  // maths: pow
#line 1587 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4890 "parser_gen.cpp"
                    break;

                    case 361:  // maths: round
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4896 "parser_gen.cpp"
                    break;

                    case 362:  // maths: sqrt
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4902 "parser_gen.cpp"
                    break;

                    case 363:  // maths: subtract
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4908 "parser_gen.cpp"
                    break;

                    case 364:  // maths: trunc
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4914 "parser_gen.cpp"
                    break;

                    case 365:  // meta: "object" META "geoNearDistance" "end of object"
#line 1592 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4922 "parser_gen.cpp"
                    break;

                    case 366:  // meta: "object" META "geoNearPoint" "end of object"
#line 1595 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4930 "parser_gen.cpp"
                    break;

                    case 367:  // meta: "object" META "indexKey" "end of object"
#line 1598 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4938 "parser_gen.cpp"
                    break;

                    case 368:  // meta: "object" META "randVal" "end of object"
#line 1601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4946 "parser_gen.cpp"
                    break;

                    case 369:  // meta: "object" META "recordId" "end of object"
#line 1604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4954 "parser_gen.cpp"
                    break;

                    case 370:  // meta: "object" META "searchHighlights" "end of object"
#line 1607 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4962 "parser_gen.cpp"
                    break;

                    case 371:  // meta: "object" META "searchScore" "end of object"
#line 1610 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4970 "parser_gen.cpp"
                    break;

                    case 372:  // meta: "object" META "sortKey" "end of object"
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4978 "parser_gen.cpp"
                    break;

                    case 373:  // meta: "object" META "textScore" "end of object"
#line 1616 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4986 "parser_gen.cpp"
                    break;

                    case 374:  // trig: sin
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4992 "parser_gen.cpp"
                    break;

                    case 375:  // trig: cos
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4998 "parser_gen.cpp"
                    break;

                    case 376:  // trig: tan
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5004 "parser_gen.cpp"
                    break;

                    case 377:  // trig: sinh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5010 "parser_gen.cpp"
                    break;

                    case 378:  // trig: cosh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5016 "parser_gen.cpp"
                    break;

                    case 379:  // trig: tanh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5022 "parser_gen.cpp"
                    break;

                    case 380:  // trig: asin
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5028 "parser_gen.cpp"
                    break;

                    case 381:  // trig: acos
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5034 "parser_gen.cpp"
                    break;

                    case 382:  // trig: atan
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5040 "parser_gen.cpp"
                    break;

                    case 383:  // trig: atan2
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5046 "parser_gen.cpp"
                    break;

                    case 384:  // trig: asinh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5052 "parser_gen.cpp"
                    break;

                    case 385:  // trig: acosh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5058 "parser_gen.cpp"
                    break;

                    case 386:  // trig: atanh
#line 1621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5064 "parser_gen.cpp"
                    break;

                    case 387:  // trig: degreesToRadians
#line 1622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5070 "parser_gen.cpp"
                    break;

                    case 388:  // trig: radiansToDegrees
#line 1622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5076 "parser_gen.cpp"
                    break;

                    case 389:  // add: "object" ADD expressionArray "end of object"
#line 1626 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5085 "parser_gen.cpp"
                    break;

                    case 390:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1633 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5094 "parser_gen.cpp"
                    break;

                    case 391:  // abs: "object" ABS expression "end of object"
#line 1639 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5102 "parser_gen.cpp"
                    break;

                    case 392:  // ceil: "object" CEIL expression "end of object"
#line 1644 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5110 "parser_gen.cpp"
                    break;

                    case 393:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1649 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5119 "parser_gen.cpp"
                    break;

                    case 394:  // exponent: "object" EXPONENT expression "end of object"
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5127 "parser_gen.cpp"
                    break;

                    case 395:  // floor: "object" FLOOR expression "end of object"
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5135 "parser_gen.cpp"
                    break;

                    case 396:  // ln: "object" LN expression "end of object"
#line 1665 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5143 "parser_gen.cpp"
                    break;

                    case 397:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1670 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5152 "parser_gen.cpp"
                    break;

                    case 398:  // logten: "object" LOGTEN expression "end of object"
#line 1676 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5160 "parser_gen.cpp"
                    break;

                    case 399:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1681 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5169 "parser_gen.cpp"
                    break;

                    case 400:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1687 "grammar.yy"
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
#line 5181 "parser_gen.cpp"
                    break;

                    case 401:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1696 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5190 "parser_gen.cpp"
                    break;

                    case 402:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1702 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5199 "parser_gen.cpp"
                    break;

                    case 403:  // sqrt: "object" SQRT expression "end of object"
#line 1708 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5207 "parser_gen.cpp"
                    break;

                    case 404:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1713 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5216 "parser_gen.cpp"
                    break;

                    case 405:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1719 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5225 "parser_gen.cpp"
                    break;

                    case 406:  // sin: "object" SIN singleArgExpression "end of object"
#line 1725 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5233 "parser_gen.cpp"
                    break;

                    case 407:  // cos: "object" COS singleArgExpression "end of object"
#line 1730 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5241 "parser_gen.cpp"
                    break;

                    case 408:  // tan: "object" TAN singleArgExpression "end of object"
#line 1735 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5249 "parser_gen.cpp"
                    break;

                    case 409:  // sinh: "object" SINH singleArgExpression "end of object"
#line 1740 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5257 "parser_gen.cpp"
                    break;

                    case 410:  // cosh: "object" COSH singleArgExpression "end of object"
#line 1745 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5265 "parser_gen.cpp"
                    break;

                    case 411:  // tanh: "object" TANH singleArgExpression "end of object"
#line 1750 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5273 "parser_gen.cpp"
                    break;

                    case 412:  // asin: "object" ASIN singleArgExpression "end of object"
#line 1755 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5281 "parser_gen.cpp"
                    break;

                    case 413:  // acos: "object" ACOS singleArgExpression "end of object"
#line 1760 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5289 "parser_gen.cpp"
                    break;

                    case 414:  // atan: "object" ATAN singleArgExpression "end of object"
#line 1765 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5297 "parser_gen.cpp"
                    break;

                    case 415:  // asinh: "object" ASINH singleArgExpression "end of object"
#line 1770 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5305 "parser_gen.cpp"
                    break;

                    case 416:  // acosh: "object" ACOSH singleArgExpression "end of object"
#line 1775 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5313 "parser_gen.cpp"
                    break;

                    case 417:  // atanh: "object" ATANH singleArgExpression "end of object"
#line 1780 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5321 "parser_gen.cpp"
                    break;

                    case 418:  // degreesToRadians: "object" DEGREES_TO_RADIANS singleArgExpression
                               // "end of object"
#line 1785 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5329 "parser_gen.cpp"
                    break;

                    case 419:  // radiansToDegrees: "object" RADIANS_TO_DEGREES singleArgExpression
                               // "end of object"
#line 1790 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5337 "parser_gen.cpp"
                    break;

                    case 420:  // boolExprs: and
#line 1796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5343 "parser_gen.cpp"
                    break;

                    case 421:  // boolExprs: or
#line 1796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5349 "parser_gen.cpp"
                    break;

                    case 422:  // boolExprs: not
#line 1796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5355 "parser_gen.cpp"
                    break;

                    case 423:  // and: "object" AND expressionArray "end of object"
#line 1800 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5364 "parser_gen.cpp"
                    break;

                    case 424:  // or: "object" OR expressionArray "end of object"
#line 1807 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5373 "parser_gen.cpp"
                    break;

                    case 425:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1814 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5382 "parser_gen.cpp"
                    break;

                    case 426:  // stringExps: concat
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5388 "parser_gen.cpp"
                    break;

                    case 427:  // stringExps: dateFromString
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5394 "parser_gen.cpp"
                    break;

                    case 428:  // stringExps: dateToString
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5400 "parser_gen.cpp"
                    break;

                    case 429:  // stringExps: indexOfBytes
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5406 "parser_gen.cpp"
                    break;

                    case 430:  // stringExps: indexOfCP
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5412 "parser_gen.cpp"
                    break;

                    case 431:  // stringExps: ltrim
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5418 "parser_gen.cpp"
                    break;

                    case 432:  // stringExps: regexFind
#line 1821 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5424 "parser_gen.cpp"
                    break;

                    case 433:  // stringExps: regexFindAll
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5430 "parser_gen.cpp"
                    break;

                    case 434:  // stringExps: regexMatch
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5436 "parser_gen.cpp"
                    break;

                    case 435:  // stringExps: replaceOne
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5442 "parser_gen.cpp"
                    break;

                    case 436:  // stringExps: replaceAll
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5448 "parser_gen.cpp"
                    break;

                    case 437:  // stringExps: rtrim
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5454 "parser_gen.cpp"
                    break;

                    case 438:  // stringExps: split
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5460 "parser_gen.cpp"
                    break;

                    case 439:  // stringExps: strLenBytes
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5466 "parser_gen.cpp"
                    break;

                    case 440:  // stringExps: strLenCP
#line 1822 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5472 "parser_gen.cpp"
                    break;

                    case 441:  // stringExps: strcasecmp
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5478 "parser_gen.cpp"
                    break;

                    case 442:  // stringExps: substr
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5484 "parser_gen.cpp"
                    break;

                    case 443:  // stringExps: substrBytes
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5490 "parser_gen.cpp"
                    break;

                    case 444:  // stringExps: substrCP
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5496 "parser_gen.cpp"
                    break;

                    case 445:  // stringExps: toLower
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5502 "parser_gen.cpp"
                    break;

                    case 446:  // stringExps: trim
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5508 "parser_gen.cpp"
                    break;

                    case 447:  // stringExps: toUpper
#line 1823 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5514 "parser_gen.cpp"
                    break;

                    case 448:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1827 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5526 "parser_gen.cpp"
                    break;

                    case 449:  // formatArg: %empty
#line 1837 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5534 "parser_gen.cpp"
                    break;

                    case 450:  // formatArg: "format argument" expression
#line 1840 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5542 "parser_gen.cpp"
                    break;

                    case 451:  // timezoneArg: %empty
#line 1846 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5550 "parser_gen.cpp"
                    break;

                    case 452:  // timezoneArg: "timezone argument" expression
#line 1849 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5558 "parser_gen.cpp"
                    break;

                    case 453:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1857 "grammar.yy"
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
#line 5568 "parser_gen.cpp"
                    break;

                    case 454:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1866 "grammar.yy"
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
#line 5578 "parser_gen.cpp"
                    break;

                    case 455:  // dateExps: dateFromParts
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5584 "parser_gen.cpp"
                    break;

                    case 456:  // dateExps: dateToParts
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5590 "parser_gen.cpp"
                    break;

                    case 457:  // dateExps: dayOfMonth
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5596 "parser_gen.cpp"
                    break;

                    case 458:  // dateExps: dayOfWeek
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5602 "parser_gen.cpp"
                    break;

                    case 459:  // dateExps: dayOfYear
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5608 "parser_gen.cpp"
                    break;

                    case 460:  // dateExps: hour
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5614 "parser_gen.cpp"
                    break;

                    case 461:  // dateExps: isoDayOfWeek
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5620 "parser_gen.cpp"
                    break;

                    case 462:  // dateExps: isoWeek
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5626 "parser_gen.cpp"
                    break;

                    case 463:  // dateExps: isoWeekYear
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5632 "parser_gen.cpp"
                    break;

                    case 464:  // dateExps: millisecond
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5638 "parser_gen.cpp"
                    break;

                    case 465:  // dateExps: minute
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5644 "parser_gen.cpp"
                    break;

                    case 466:  // dateExps: month
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5650 "parser_gen.cpp"
                    break;

                    case 467:  // dateExps: second
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5656 "parser_gen.cpp"
                    break;

                    case 468:  // dateExps: week
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5662 "parser_gen.cpp"
                    break;

                    case 469:  // dateExps: year
#line 1875 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5668 "parser_gen.cpp"
                    break;

                    case 470:  // hourArg: %empty
#line 1879 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
                    }
#line 5676 "parser_gen.cpp"
                    break;

                    case 471:  // hourArg: "hour argument" expression
#line 1882 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::hourArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5684 "parser_gen.cpp"
                    break;

                    case 472:  // minuteArg: %empty
#line 1888 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
                    }
#line 5692 "parser_gen.cpp"
                    break;

                    case 473:  // minuteArg: "minute argument" expression
#line 1891 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::minuteArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5700 "parser_gen.cpp"
                    break;

                    case 474:  // secondArg: %empty
#line 1897 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
                    }
#line 5708 "parser_gen.cpp"
                    break;

                    case 475:  // secondArg: "second argument" expression
#line 1900 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::secondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5716 "parser_gen.cpp"
                    break;

                    case 476:  // millisecondArg: %empty
#line 1906 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
                    }
#line 5724 "parser_gen.cpp"
                    break;

                    case 477:  // millisecondArg: "millisecond argument" expression
#line 1909 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::millisecondArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5732 "parser_gen.cpp"
                    break;

                    case 478:  // dayArg: %empty
#line 1915 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
                    }
#line 5740 "parser_gen.cpp"
                    break;

                    case 479:  // dayArg: "day argument" expression
#line 1918 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::dayArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5748 "parser_gen.cpp"
                    break;

                    case 480:  // isoDayOfWeekArg: %empty
#line 1924 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5756 "parser_gen.cpp"
                    break;

                    case 481:  // isoDayOfWeekArg: "ISO day of week argument" expression
#line 1927 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoDayOfWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5764 "parser_gen.cpp"
                    break;

                    case 482:  // isoWeekArg: %empty
#line 1933 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
                    }
#line 5772 "parser_gen.cpp"
                    break;

                    case 483:  // isoWeekArg: "ISO week argument" expression
#line 1936 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::isoWeekArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5780 "parser_gen.cpp"
                    break;

                    case 484:  // iso8601Arg: %empty
#line 1942 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
                    }
#line 5788 "parser_gen.cpp"
                    break;

                    case 485:  // iso8601Arg: "ISO 8601 argument" bool
#line 1945 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::iso8601Arg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5796 "parser_gen.cpp"
                    break;

                    case 486:  // monthArg: %empty
#line 1951 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
                    }
#line 5804 "parser_gen.cpp"
                    break;

                    case 487:  // monthArg: "month argument" expression
#line 1954 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::monthArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5812 "parser_gen.cpp"
                    break;

                    case 488:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg millisecondArg minuteArg monthArg secondArg
                               // timezoneArg "year argument" expression "end of object" "end of
                               // object"
#line 1961 "grammar.yy"
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
#line 5822 "parser_gen.cpp"
                    break;

                    case 489:  // dateFromParts: "object" DATE_FROM_PARTS START_ORDERED_OBJECT
                               // dayArg hourArg isoDayOfWeekArg isoWeekArg "ISO week year argument"
                               // expression millisecondArg minuteArg monthArg secondArg timezoneArg
                               // "end of object" "end of object"
#line 1967 "grammar.yy"
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
#line 5832 "parser_gen.cpp"
                    break;

                    case 490:  // dateToParts: "object" DATE_TO_PARTS START_ORDERED_OBJECT "date
                               // argument" expression iso8601Arg timezoneArg "end of object" "end
                               // of object"
#line 1975 "grammar.yy"
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
#line 5842 "parser_gen.cpp"
                    break;

                    case 491:  // dayOfMonth: "object" DAY_OF_MONTH nonArrayNonObjExpression "end of
                               // object"
#line 1983 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5850 "parser_gen.cpp"
                    break;

                    case 492:  // dayOfMonth: "object" DAY_OF_MONTH START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 1986 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5859 "parser_gen.cpp"
                    break;

                    case 493:  // dayOfMonth: "object" DAY_OF_MONTH expressionSingletonArray "end of
                               // object"
#line 1990 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfMonth, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5867 "parser_gen.cpp"
                    break;

                    case 494:  // dayOfWeek: "object" DAY_OF_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 1996 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5876 "parser_gen.cpp"
                    break;

                    case 495:  // dayOfWeek: "object" DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2000 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5885 "parser_gen.cpp"
                    break;

                    case 496:  // dayOfWeek: "object" DAY_OF_WEEK expressionSingletonArray "end of
                               // object"
#line 2004 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5893 "parser_gen.cpp"
                    break;

                    case 497:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK nonArrayNonObjExpression
                               // "end of object"
#line 2010 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5902 "parser_gen.cpp"
                    break;

                    case 498:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2014 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5911 "parser_gen.cpp"
                    break;

                    case 499:  // isoDayOfWeek: "object" ISO_DAY_OF_WEEK expressionSingletonArray
                               // "end of object"
#line 2018 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoDayOfWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5919 "parser_gen.cpp"
                    break;

                    case 500:  // dayOfYear: "object" DAY_OF_YEAR nonArrayNonObjExpression "end of
                               // object"
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5928 "parser_gen.cpp"
                    break;

                    case 501:  // dayOfYear: "object" DAY_OF_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2028 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5937 "parser_gen.cpp"
                    break;

                    case 502:  // dayOfYear: "object" DAY_OF_YEAR expressionSingletonArray "end of
                               // object"
#line 2032 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::dayOfYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5945 "parser_gen.cpp"
                    break;

                    case 503:  // hour: "object" HOUR nonArrayNonObjExpression "end of object"
#line 2038 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5954 "parser_gen.cpp"
                    break;

                    case 504:  // hour: "object" HOUR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2042 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5963 "parser_gen.cpp"
                    break;

                    case 505:  // hour: "object" HOUR expressionSingletonArray "end of object"
#line 2046 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::hour, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5971 "parser_gen.cpp"
                    break;

                    case 506:  // month: "object" MONTH nonArrayNonObjExpression "end of object"
#line 2052 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 5980 "parser_gen.cpp"
                    break;

                    case 507:  // month: "object" MONTH START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2056 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5989 "parser_gen.cpp"
                    break;

                    case 508:  // month: "object" MONTH expressionSingletonArray "end of object"
#line 2060 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::month, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5997 "parser_gen.cpp"
                    break;

                    case 509:  // week: "object" WEEK nonArrayNonObjExpression "end of object"
#line 2066 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6006 "parser_gen.cpp"
                    break;

                    case 510:  // week: "object" WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2070 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6015 "parser_gen.cpp"
                    break;

                    case 511:  // week: "object" WEEK expressionSingletonArray "end of object"
#line 2074 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::week, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6023 "parser_gen.cpp"
                    break;

                    case 512:  // isoWeek: "object" ISO_WEEK nonArrayNonObjExpression "end of
                               // object"
#line 2080 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6032 "parser_gen.cpp"
                    break;

                    case 513:  // isoWeek: "object" ISO_WEEK START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2084 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6041 "parser_gen.cpp"
                    break;

                    case 514:  // isoWeek: "object" ISO_WEEK expressionSingletonArray "end of
                               // object"
#line 2088 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeek, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6049 "parser_gen.cpp"
                    break;

                    case 515:  // isoWeekYear: "object" ISO_WEEK_YEAR nonArrayNonObjExpression "end
                               // of object"
#line 2094 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6058 "parser_gen.cpp"
                    break;

                    case 516:  // isoWeekYear: "object" ISO_WEEK_YEAR START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2098 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6067 "parser_gen.cpp"
                    break;

                    case 517:  // isoWeekYear: "object" ISO_WEEK_YEAR expressionSingletonArray "end
                               // of object"
#line 2102 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::isoWeekYear, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6075 "parser_gen.cpp"
                    break;

                    case 518:  // year: "object" YEAR nonArrayNonObjExpression "end of object"
#line 2108 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6084 "parser_gen.cpp"
                    break;

                    case 519:  // year: "object" YEAR START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2112 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6093 "parser_gen.cpp"
                    break;

                    case 520:  // year: "object" YEAR expressionSingletonArray "end of object"
#line 2116 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::year, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6101 "parser_gen.cpp"
                    break;

                    case 521:  // second: "object" SECOND nonArrayNonObjExpression "end of object"
#line 2122 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6110 "parser_gen.cpp"
                    break;

                    case 522:  // second: "object" SECOND START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2126 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6119 "parser_gen.cpp"
                    break;

                    case 523:  // second: "object" SECOND expressionSingletonArray "end of object"
#line 2130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::second, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6127 "parser_gen.cpp"
                    break;

                    case 524:  // millisecond: "object" MILLISECOND nonArrayNonObjExpression "end of
                               // object"
#line 2136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6136 "parser_gen.cpp"
                    break;

                    case 525:  // millisecond: "object" MILLISECOND START_ORDERED_OBJECT "date
                               // argument" expression timezoneArg "end of object" "end of object"
#line 2140 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6145 "parser_gen.cpp"
                    break;

                    case 526:  // millisecond: "object" MILLISECOND expressionSingletonArray "end of
                               // object"
#line 2144 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::millisecond, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6153 "parser_gen.cpp"
                    break;

                    case 527:  // minute: "object" MINUTE nonArrayNonObjExpression "end of object"
#line 2150 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};

                    }
#line 6162 "parser_gen.cpp"
                    break;

                    case 528:  // minute: "object" MINUTE START_ORDERED_OBJECT "date argument"
                               // expression timezoneArg "end of object" "end of object"
#line 2154 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::dateArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                 YY_MOVE(yystack_[2]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6171 "parser_gen.cpp"
                    break;

                    case 529:  // minute: "object" MINUTE expressionSingletonArray "end of object"
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::minute, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6179 "parser_gen.cpp"
                    break;

                    case 530:  // exprZeroToTwo: %empty
#line 2164 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 6187 "parser_gen.cpp"
                    break;

                    case 531:  // exprZeroToTwo: expression
#line 2167 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6195 "parser_gen.cpp"
                    break;

                    case 532:  // exprZeroToTwo: expression expression
#line 2170 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6203 "parser_gen.cpp"
                    break;

                    case 533:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 2177 "grammar.yy"
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
#line 6215 "parser_gen.cpp"
                    break;

                    case 534:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 2188 "grammar.yy"
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
#line 6227 "parser_gen.cpp"
                    break;

                    case 535:  // charsArg: %empty
#line 2198 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 6235 "parser_gen.cpp"
                    break;

                    case 536:  // charsArg: "chars argument" expression
#line 2201 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6243 "parser_gen.cpp"
                    break;

                    case 537:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2207 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6253 "parser_gen.cpp"
                    break;

                    case 538:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 2215 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6263 "parser_gen.cpp"
                    break;

                    case 539:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 2223 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 6273 "parser_gen.cpp"
                    break;

                    case 540:  // optionsArg: %empty
#line 2231 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 6281 "parser_gen.cpp"
                    break;

                    case 541:  // optionsArg: "options argument" expression
#line 2234 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6289 "parser_gen.cpp"
                    break;

                    case 542:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 2239 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 6301 "parser_gen.cpp"
                    break;

                    case 543:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 2248 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6309 "parser_gen.cpp"
                    break;

                    case 544:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 2254 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6317 "parser_gen.cpp"
                    break;

                    case 545:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 2260 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6325 "parser_gen.cpp"
                    break;

                    case 546:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2267 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6336 "parser_gen.cpp"
                    break;

                    case 547:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 2277 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 6347 "parser_gen.cpp"
                    break;

                    case 548:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 2286 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6356 "parser_gen.cpp"
                    break;

                    case 549:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 2293 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6365 "parser_gen.cpp"
                    break;

                    case 550:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 2300 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6374 "parser_gen.cpp"
                    break;

                    case 551:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 2308 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6383 "parser_gen.cpp"
                    break;

                    case 552:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 2316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6392 "parser_gen.cpp"
                    break;

                    case 553:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 2324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6401 "parser_gen.cpp"
                    break;

                    case 554:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 2332 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6410 "parser_gen.cpp"
                    break;

                    case 555:  // toLower: "object" TO_LOWER expression "end of object"
#line 2339 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6418 "parser_gen.cpp"
                    break;

                    case 556:  // toUpper: "object" TO_UPPER expression "end of object"
#line 2345 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6426 "parser_gen.cpp"
                    break;

                    case 557:  // metaSortKeyword: "randVal"
#line 2351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 6434 "parser_gen.cpp"
                    break;

                    case 558:  // metaSortKeyword: "textScore"
#line 2354 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 6442 "parser_gen.cpp"
                    break;

                    case 559:  // metaSort: "object" META metaSortKeyword "end of object"
#line 2360 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6450 "parser_gen.cpp"
                    break;

                    case 560:  // sortSpecs: "object" specList "end of object"
#line 2366 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6458 "parser_gen.cpp"
                    break;

                    case 561:  // specList: %empty
#line 2371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6466 "parser_gen.cpp"
                    break;

                    case 562:  // specList: specList sortSpec
#line 2374 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6475 "parser_gen.cpp"
                    break;

                    case 563:  // oneOrNegOne: "1 (int)"
#line 2381 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 6483 "parser_gen.cpp"
                    break;

                    case 564:  // oneOrNegOne: "-1 (int)"
#line 2384 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 6491 "parser_gen.cpp"
                    break;

                    case 565:  // oneOrNegOne: "1 (long)"
#line 2387 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 6499 "parser_gen.cpp"
                    break;

                    case 566:  // oneOrNegOne: "-1 (long)"
#line 2390 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 6507 "parser_gen.cpp"
                    break;

                    case 567:  // oneOrNegOne: "1 (double)"
#line 2393 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 6515 "parser_gen.cpp"
                    break;

                    case 568:  // oneOrNegOne: "-1 (double)"
#line 2396 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 6523 "parser_gen.cpp"
                    break;

                    case 569:  // oneOrNegOne: "1 (decimal)"
#line 2399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 6531 "parser_gen.cpp"
                    break;

                    case 570:  // oneOrNegOne: "-1 (decimal)"
#line 2402 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 6539 "parser_gen.cpp"
                    break;

                    case 571:  // sortFieldname: valueFieldname
#line 2407 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 6547 "parser_gen.cpp"
                    break;

                    case 572:  // sortFieldname: "fieldname containing dotted path"
#line 2409 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 6559 "parser_gen.cpp"
                    break;

                    case 573:  // sortSpec: sortFieldname metaSort
#line 2419 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6567 "parser_gen.cpp"
                    break;

                    case 574:  // sortSpec: sortFieldname oneOrNegOne
#line 2421 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6575 "parser_gen.cpp"
                    break;

                    case 575:  // setExpression: allElementsTrue
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6581 "parser_gen.cpp"
                    break;

                    case 576:  // setExpression: anyElementTrue
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6587 "parser_gen.cpp"
                    break;

                    case 577:  // setExpression: setDifference
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6593 "parser_gen.cpp"
                    break;

                    case 578:  // setExpression: setEquals
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6599 "parser_gen.cpp"
                    break;

                    case 579:  // setExpression: setIntersection
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6605 "parser_gen.cpp"
                    break;

                    case 580:  // setExpression: setIsSubset
#line 2427 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6611 "parser_gen.cpp"
                    break;

                    case 581:  // setExpression: setUnion
#line 2428 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6617 "parser_gen.cpp"
                    break;

                    case 582:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 2432 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6625 "parser_gen.cpp"
                    break;

                    case 583:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 2438 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 6633 "parser_gen.cpp"
                    break;

                    case 584:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 2444 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6642 "parser_gen.cpp"
                    break;

                    case 585:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2452 "grammar.yy"
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
#line 6654 "parser_gen.cpp"
                    break;

                    case 586:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 2463 "grammar.yy"
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
#line 6666 "parser_gen.cpp"
                    break;

                    case 587:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 2473 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6675 "parser_gen.cpp"
                    break;

                    case 588:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2481 "grammar.yy"
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
#line 6687 "parser_gen.cpp"
                    break;

                    case 589:  // literalEscapes: const
#line 2491 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6693 "parser_gen.cpp"
                    break;

                    case 590:  // literalEscapes: literal
#line 2491 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6699 "parser_gen.cpp"
                    break;

                    case 591:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 2495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6708 "parser_gen.cpp"
                    break;

                    case 592:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 2502 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 6717 "parser_gen.cpp"
                    break;

                    case 593:  // value: simpleValue
#line 2509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6723 "parser_gen.cpp"
                    break;

                    case 594:  // value: compoundValue
#line 2509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6729 "parser_gen.cpp"
                    break;

                    case 595:  // compoundValue: valueArray
#line 2513 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6735 "parser_gen.cpp"
                    break;

                    case 596:  // compoundValue: valueObject
#line 2513 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6741 "parser_gen.cpp"
                    break;

                    case 597:  // valueArray: "array" values "end of array"
#line 2517 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 6749 "parser_gen.cpp"
                    break;

                    case 598:  // values: %empty
#line 2523 "grammar.yy"
                    {
                    }
#line 6755 "parser_gen.cpp"
                    break;

                    case 599:  // values: values value
#line 2524 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 6764 "parser_gen.cpp"
                    break;

                    case 600:  // valueObject: "object" valueFields "end of object"
#line 2531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 6772 "parser_gen.cpp"
                    break;

                    case 601:  // valueFields: %empty
#line 2537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 6780 "parser_gen.cpp"
                    break;

                    case 602:  // valueFields: valueFields valueField
#line 2540 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 6789 "parser_gen.cpp"
                    break;

                    case 603:  // valueField: valueFieldname value
#line 2547 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6797 "parser_gen.cpp"
                    break;

                    case 604:  // valueFieldname: invariableUserFieldname
#line 2554 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 6803 "parser_gen.cpp"
                    break;

                    case 605:  // valueFieldname: stageAsUserFieldname
#line 2555 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 6809 "parser_gen.cpp"
                    break;

                    case 606:  // valueFieldname: argAsUserFieldname
#line 2556 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 6815 "parser_gen.cpp"
                    break;

                    case 607:  // valueFieldname: aggExprAsUserFieldname
#line 2557 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 6821 "parser_gen.cpp"
                    break;

                    case 608:  // valueFieldname: idAsUserFieldname
#line 2558 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 6827 "parser_gen.cpp"
                    break;

                    case 609:  // compExprs: cmp
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6833 "parser_gen.cpp"
                    break;

                    case 610:  // compExprs: eq
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6839 "parser_gen.cpp"
                    break;

                    case 611:  // compExprs: gt
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6845 "parser_gen.cpp"
                    break;

                    case 612:  // compExprs: gte
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6851 "parser_gen.cpp"
                    break;

                    case 613:  // compExprs: lt
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6857 "parser_gen.cpp"
                    break;

                    case 614:  // compExprs: lte
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6863 "parser_gen.cpp"
                    break;

                    case 615:  // compExprs: ne
#line 2561 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6869 "parser_gen.cpp"
                    break;

                    case 616:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 2563 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6878 "parser_gen.cpp"
                    break;

                    case 617:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 2568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6887 "parser_gen.cpp"
                    break;

                    case 618:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 2573 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6896 "parser_gen.cpp"
                    break;

                    case 619:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 2578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6905 "parser_gen.cpp"
                    break;

                    case 620:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 2583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6914 "parser_gen.cpp"
                    break;

                    case 621:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 2588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6923 "parser_gen.cpp"
                    break;

                    case 622:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 2593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6932 "parser_gen.cpp"
                    break;

                    case 623:  // typeExpression: convert
#line 2599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6938 "parser_gen.cpp"
                    break;

                    case 624:  // typeExpression: toBool
#line 2600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6944 "parser_gen.cpp"
                    break;

                    case 625:  // typeExpression: toDate
#line 2601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6950 "parser_gen.cpp"
                    break;

                    case 626:  // typeExpression: toDecimal
#line 2602 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6956 "parser_gen.cpp"
                    break;

                    case 627:  // typeExpression: toDouble
#line 2603 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6962 "parser_gen.cpp"
                    break;

                    case 628:  // typeExpression: toInt
#line 2604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6968 "parser_gen.cpp"
                    break;

                    case 629:  // typeExpression: toLong
#line 2605 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6974 "parser_gen.cpp"
                    break;

                    case 630:  // typeExpression: toObjectId
#line 2606 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6980 "parser_gen.cpp"
                    break;

                    case 631:  // typeExpression: toString
#line 2607 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6986 "parser_gen.cpp"
                    break;

                    case 632:  // typeExpression: type
#line 2608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6992 "parser_gen.cpp"
                    break;

                    case 633:  // onErrorArg: %empty
#line 2613 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 7000 "parser_gen.cpp"
                    break;

                    case 634:  // onErrorArg: "onError argument" expression
#line 2616 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7008 "parser_gen.cpp"
                    break;

                    case 635:  // onNullArg: %empty
#line 2623 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 7016 "parser_gen.cpp"
                    break;

                    case 636:  // onNullArg: "onNull argument" expression
#line 2626 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 7024 "parser_gen.cpp"
                    break;

                    case 637:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2633 "grammar.yy"
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
#line 7035 "parser_gen.cpp"
                    break;

                    case 638:  // toBool: "object" TO_BOOL expression "end of object"
#line 2642 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7043 "parser_gen.cpp"
                    break;

                    case 639:  // toDate: "object" TO_DATE expression "end of object"
#line 2647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7051 "parser_gen.cpp"
                    break;

                    case 640:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2652 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7059 "parser_gen.cpp"
                    break;

                    case 641:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 2657 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7067 "parser_gen.cpp"
                    break;

                    case 642:  // toInt: "object" TO_INT expression "end of object"
#line 2662 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7075 "parser_gen.cpp"
                    break;

                    case 643:  // toLong: "object" TO_LONG expression "end of object"
#line 2667 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7083 "parser_gen.cpp"
                    break;

                    case 644:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 2672 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7091 "parser_gen.cpp"
                    break;

                    case 645:  // toString: "object" TO_STRING expression "end of object"
#line 2677 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7099 "parser_gen.cpp"
                    break;

                    case 646:  // type: "object" TYPE expression "end of object"
#line 2682 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 7107 "parser_gen.cpp"
                    break;


#line 7111 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -1017;

const short ParserGen::yytable_ninf_ = -481;

const short ParserGen::yypact_[] = {
    -127,  -115,  -110,  -107,  37,    -94,   -1017, -1017, -1017, -1017, -1017, -1017, 136,
    -17,   1502,  846,   -82,   79,    -73,   -69,   79,    -58,   15,    -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, 2889,  -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, 3477,  -1017, -1017, -1017, -1017, -55,   -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, 72,    -1017, -1017, -1017, 17,    -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, 65,    -1017, -1017, 91,    -94,   -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, 10,    -1017, -1017,
    -1017, 1533,  79,    76,    -1017, 1713,  1178,  -47,   -110,  -90,   -1017, 3624,  -1017,
    -1017, 3624,  -1017, -1017, -1017, -1017, 18,    82,    -1017, -1017, -1017, 2889,  -1017,
    -1017, 2889,  -128,  3754,  -1017, -1017, -1017, -1017, 45,    -1017, -1017, 46,    -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, 1012,  -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -37,   -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, 1860,  3036,  3183,  3183,  -20,   -19,   -20,
    -16,   3183,  3183,  3183,  -5,    3183,  3036,  -5,    2,     3,     -58,   3183,  3183,
    -58,   -58,   -58,   -58,   3330,  3330,  3330,  3183,  7,     -5,    3036,  3036,  -5,
    -5,    3330,  -1017, 9,     12,    3330,  3330,  3330,  13,    3036,  14,    3036,  -5,
    -5,    -58,   148,   3330,  3330,  19,    3330,  20,    -5,    28,    -20,   29,    3183,
    -58,   -58,   -58,   -58,   -58,   35,    -58,   3330,  -5,    39,    40,    -5,    41,
    42,    3183,  3183,  44,    3036,  49,    3036,  3036,  54,    55,    58,    59,    3183,
    3183,  3036,  3036,  3036,  3036,  3036,  3036,  3036,  3036,  3036,  3036,  -58,   60,
    3036,  3330,  3330,  3624,  3624,  -1017, 1603,  62,    -66,   3796,  -1017, 1343,  -1017,
    -1017, -1017, -1017, -1017, 67,    3036,  -1017, -1017, -1017, -1017, -1017, -1017, 88,
    124,   137,   3036,  139,   3036,  141,   143,   145,   3036,  146,   147,   151,   152,
    -1017, 2889,  104,   155,   162,   113,   204,   220,   221,   1343,  -1017, -1017, 166,
    168,   226,   175,   178,   236,   180,   181,   241,   185,   3036,  186,   188,   189,
    192,   197,   198,   207,   269,   3036,  3036,  213,   214,   274,   219,   223,   277,
    224,   231,   278,   2889,  233,   3036,  234,   239,   242,   303,   246,   247,   251,
    252,   254,   255,   260,   261,   265,   270,   280,   335,   281,   284,   342,   3036,
    290,   295,   353,   3036,  300,   3036,  304,   3036,  307,   311,   362,   313,   315,
    370,   371,   3036,  303,   332,   337,   392,   341,   3036,  3036,  347,   3036,  3036,
    351,   352,   354,   355,   3036,  356,   3036,  357,   358,   3036,  3036,  3036,  3036,
    359,   361,   363,   364,   365,   366,   367,   368,   369,   372,   373,   374,   303,
    3036,  375,   376,   377,   399,   379,   380,   419,   -1017, -1017, -1017, -1017, -1017,
    381,   -1017, -1017, 1693,  -1017, 383,   -1017, -1017, -1017, 384,   -1017, 385,   -1017,
    -1017, -1017, 3036,  -1017, -1017, -1017, -1017, 2007,  386,   3036,  -1017, -1017, 3036,
    437,   3036,  3036,  3036,  -1017, -1017, 3036,  -1017, -1017, 3036,  -1017, -1017, 3036,
    -1017, 3036,  -1017, -1017, -1017, -1017, -1017, -1017, -1017, 3036,  3036,  3036,  -1017,
    -1017, 3036,  -1017, -1017, 3036,  -1017, -1017, 3036,  388,   -1017, 3036,  -1017, -1017,
    -1017, 3036,  438,   -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, 3036,  -1017, -1017, 3036,  3036,  -1017, -1017, 3036,  3036,  -1017, 390,   -1017,
    3036,  -1017, -1017, 3036,  -1017, -1017, 3036,  3036,  3036,  441,   -1017, -1017, 3036,
    -1017, 3036,  3036,  -1017, 3036,  3036,  -1017, -1017, -1017, -1017, 3036,  -1017, 3036,
    -1017, -1017, 3036,  3036,  3036,  3036,  -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, 442,   3036,  -1017, -1017, -1017, 3036,  -1017, -1017,
    3036,  -1017, -1017, 3036,  -1017, -1017, -1017, -1017, -1017, -1017, 393,   394,   398,
    401,   403,   446,   -1017, 3036,  53,    457,   455,   457,   443,   443,   443,   409,
    443,   3036,  3036,  443,   443,   443,   410,   412,   -1017, 3036,  443,   443,   414,
    443,   -1017, 417,   415,   458,   470,   471,   424,   3036,  443,   -1017, -1017, -1017,
    2154,  428,   433,   3036,  3036,  3036,  434,   3036,  444,   443,   443,   -1017, -1017,
    -1017, -1017, -1017, -1017, 3036,  478,   -1017, 3036,  3036,  484,   492,   3036,  443,
    -4,    443,   443,   3036,  450,   451,   452,   454,   456,   3036,  459,   460,   461,
    462,   464,   -1017, 469,   473,   475,   476,   477,   479,   2301,  -1017, 480,   3036,
    493,   3036,  3036,  481,   483,   486,   2448,  2595,  2742,  472,   491,   497,   494,
    500,   501,   503,   506,   507,   508,   509,   -1017, 3036,  490,   -1017, -1017, 3036,
    513,   3036,  524,   -1017, 446,   -1017, 510,   478,   -1017, 511,   512,   514,   -1017,
    517,   -1017, 518,   519,   520,   521,   526,   -1017, 527,   531,   532,   -1017, 533,
    534,   -1017, -1017, 3036,  529,   538,   -1017, 540,   541,   542,   543,   549,   -1017,
    -1017, -1017, 550,   555,   556,   -1017, 557,   -1017, 558,   563,   -1017, 3036,  -1017,
    3036,  574,   -1017, 3036,  478,   564,   568,   -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, 569,   3036,  3036,  -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, 572,   -1017, 3036,  443,
    618,   575,   -1017, 578,   -1017, 581,   582,   583,   -1017, 603,   484,   584,   -1017,
    586,   587,   -1017, 3036,  513,   -1017, -1017, -1017, 588,   574,   589,   443,   -1017,
    590,   591,   -1017};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   74,  3,   561, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   9,   10,  11,  12,  13,  14,  5,   98,  126, 115, 125, 122, 136, 129, 123,
    131, 118, 140, 137, 138, 139, 135, 133, 141, 120, 121, 128, 116, 127, 130, 134, 117, 124, 119,
    132, 0,   73,  347, 100, 99,  106, 104, 105, 103, 0,   113, 75,  77,  78,  0,   166, 234, 237,
    142, 220, 144, 221, 233, 236, 235, 143, 238, 167, 149, 182, 145, 156, 228, 231, 183, 198, 184,
    199, 185, 186, 187, 239, 168, 560, 150, 169, 170, 151, 152, 188, 200, 201, 189, 190, 191, 146,
    171, 172, 173, 153, 154, 202, 203, 192, 193, 174, 194, 175, 155, 148, 147, 176, 240, 204, 205,
    206, 208, 207, 177, 209, 195, 222, 223, 224, 225, 226, 178, 227, 230, 210, 179, 107, 110, 111,
    112, 109, 108, 213, 211, 212, 214, 215, 216, 180, 229, 232, 157, 158, 159, 160, 161, 162, 217,
    163, 164, 219, 218, 181, 165, 196, 197, 572, 605, 606, 607, 604, 0,   608, 571, 562, 0,   281,
    280, 279, 277, 276, 275, 269, 268, 267, 273, 272, 271, 266, 270, 274, 278, 18,  19,  20,  21,
    23,  25,  0,   22,  8,   0,   6,   283, 282, 242, 243, 244, 245, 246, 247, 248, 249, 598, 601,
    250, 241, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 293, 294,
    295, 296, 297, 302, 298, 299, 300, 303, 304, 94,  284, 285, 287, 288, 289, 301, 290, 291, 292,
    593, 594, 595, 596, 286, 81,  79,  76,  101, 570, 569, 568, 567, 564, 563, 566, 565, 0,   573,
    574, 16,  0,   0,   0,   7,   0,   0,   0,   0,   0,   24,  0,   65,  67,  0,   64,  66,  26,
    114, 0,   0,   597, 599, 600, 0,   602, 80,  0,   0,   0,   82,  83,  84,  85,  102, 557, 558,
    0,   58,  57,  54,  53,  56,  50,  49,  52,  42,  41,  44,  46,  45,  48,  305, 0,   43,  47,
    51,  55,  37,  38,  39,  40,  59,  60,  61,  30,  31,  32,  33,  34,  35,  36,  27,  29,  62,
    63,  317, 332, 330, 318, 319, 349, 320, 420, 421, 422, 321, 589, 590, 324, 426, 427, 428, 429,
    430, 431, 432, 433, 434, 435, 436, 437, 438, 439, 440, 441, 442, 443, 444, 445, 447, 446, 322,
    609, 610, 611, 612, 613, 614, 615, 327, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 465,
    466, 467, 468, 469, 323, 623, 624, 625, 626, 627, 628, 629, 630, 631, 632, 350, 351, 352, 353,
    354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 325, 575, 576, 577, 578, 579, 580, 581,
    326, 374, 375, 376, 377, 378, 379, 380, 381, 382, 384, 385, 386, 383, 387, 388, 331, 28,  15,
    0,   603, 86,  81,  95,  88,  91,  93,  92,  90,  97,  559, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   348, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   69,  0,   0,   0,   0,   335, 340, 307, 306, 309, 308, 310, 0,   0,   311, 315, 337, 312,
    316, 338, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   305, 0,   0,
    0,   0,   478, 0,   0,   0,   8,   313, 314, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   535, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   535, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   535,
    0,   0,   0,   0,   0,   0,   0,   0,   72,  71,  68,  70,  17,  82,  87,  89,  0,   391, 0,
    413, 416, 389, 0,   423, 0,   412, 415, 414, 0,   390, 417, 392, 616, 0,   0,   0,   407, 410,
    0,   470, 0,   0,   0,   493, 491, 0,   496, 494, 0,   502, 500, 0,   418, 0,   617, 394, 395,
    618, 619, 505, 503, 0,   0,   0,   499, 497, 0,   514, 512, 0,   517, 515, 0,   0,   396, 0,
    398, 620, 621, 0,   0,   365, 366, 367, 368, 369, 370, 371, 372, 373, 526, 524, 0,   529, 527,
    0,   0,   508, 506, 0,   0,   622, 0,   424, 0,   419, 543, 0,   544, 545, 0,   0,   0,   0,
    523, 521, 0,   584, 0,   0,   587, 0,   0,   333, 334, 406, 409, 0,   403, 0,   549, 550, 0,
    0,   0,   0,   408, 411, 638, 639, 640, 641, 642, 643, 555, 644, 645, 556, 0,   0,   646, 511,
    509, 0,   520, 518, 0,   96,  339, 0,   344, 345, 343, 346, 341, 336, 0,   0,   0,   0,   0,
    633, 479, 0,   476, 449, 484, 449, 451, 451, 451, 0,   451, 530, 530, 451, 451, 451, 0,   0,
    536, 0,   451, 451, 0,   451, 305, 0,   0,   540, 0,   0,   0,   0,   451, 305, 305, 305, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   451, 451, 342, 582, 583, 328, 448, 591, 0,   635, 471,
    0,   0,   472, 482, 0,   451, 0,   451, 451, 0,   0,   0,   0,   0,   0,   531, 0,   0,   0,
    0,   0,   592, 0,   0,   0,   0,   0,   0,   0,   425, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   634, 0,   0,   481, 477,
    0,   486, 0,   0,   450, 633, 485, 0,   635, 452, 0,   0,   0,   393, 0,   532, 0,   0,   0,
    0,   0,   397, 0,   0,   0,   399, 0,   0,   401, 541, 0,   0,   0,   402, 0,   0,   0,   0,
    0,   329, 548, 551, 0,   0,   0,   404, 0,   405, 0,   0,   636, 0,   473, 0,   474, 483, 0,
    635, 0,   0,   492, 495, 501, 504, 533, 534, 498, 513, 516, 537, 525, 528, 507, 400, 0,   0,
    0,   538, 522, 585, 586, 588, 552, 553, 554, 539, 510, 519, 0,   487, 0,   451, 476, 0,   490,
    0,   542, 0,   0,   0,   475, 0,   472, 0,   454, 0,   0,   637, 0,   486, 453, 547, 546, 0,
    474, 0,   451, 488, 0,   0,   489};

const short ParserGen::yypgoto_[] = {
    -1017, 378,   -1017, -1017, -96,   -14,   -1017, -1017, -13,   -1017, -12,   -1017, 387,
    -1017, -1017, 73,    -1017, -1017, -281,  -243,  -231,  -227,  -218,  -9,    -196,  -3,
    -10,   -1,    -194,  -192,  -287,  -262,  -1017, -190,  -186,  -181,  -312,  -175,  -171,
    -252,  109,   -1017, -1017, -1017, -1017, -1017, -1017, 84,    -1017, 467,   -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, 397,   -546,  -1017, -8,    -485,  -1017,
    -25,   -1017, -1017, -1017, -248,  68,    -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -492,  -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -365,  -1016, -251,  -423,  -669,  -1017, -1017, -445,  -456,  -432,  -1017, -1017, -1017,
    -448,  -1017, -604,  -1017, -237,  -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, -1017, 259,   1149,  865,   -335,  405,   -1017, 215,   -1017,
    -1017, -1017, -1017, 99,    -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017, -1017,
    -1017, -1017, -1017, -1017, 164};

const short ParserGen::yydefgoto_[] = {
    -1,  588,  301,  900, 180, 181,  302, 182,  183, 184, 185, 589, 186, 67,   303, 590, 905, 311,
    68,  245,  246,  247, 248, 249,  250, 251,  252, 253, 254, 255, 256, 257,  258, 259, 260, 261,
    262, 263,  264,  265, 597, 267,  268, 269,  292, 270, 482, 483, 6,   13,   22,  23,  24,  25,
    26,  27,   28,   287, 591, 358,  359, 360,  484, 598, 361, 620, 716, 362,  363, 599, 600, 757,
    365, 366,  367,  368, 369, 370,  371, 372,  373, 374, 375, 376, 377, 378,  379, 380, 381, 382,
    383, 384,  698,  385, 386, 387,  388, 389,  390, 391, 392, 393, 394, 395,  396, 397, 398, 399,
    400, 401,  402,  403, 404, 405,  406, 407,  408, 409, 410, 411, 412, 413,  414, 415, 416, 417,
    418, 419,  420,  421, 422, 423,  424, 425,  426, 427, 428, 429, 430, 431,  432, 433, 434, 435,
    436, 437,  438,  439, 440, 441,  442, 443,  444, 445, 446, 447, 967, 1023, 974, 979, 821, 1001,
    915, 1027, 1117, 971, 780, 1029, 976, 1080, 972, 487, 291, 985, 448, 449,  450, 451, 452, 453,
    454, 455,  456,  457, 458, 459,  460, 461,  462, 463, 464, 465, 466, 467,  468, 469, 470, 471,
    606, 607,  601,  609, 610, 637,  8,   14,   293, 273, 294, 69,  70,  316,  317, 318, 319, 71,
    72,  594,  10,   15,  284, 285,  323, 187,  4,   699};

const short ParserGen::yytable_[] = {
    64,   65,   66,   602,  308,  313,  304,  207,  205,  209,  207,  205,  212,  478,  206,  622,
    208,  206,  1085, 208,  774,  321,  312,  476,  313,  5,    477,  306,  192,  193,  194,  7,
    648,  649,  9,    351,  854,  11,   351,  216,  217,  314,  749,  750,  667,  357,  669,  12,
    357,  364,  237,  29,   364,  481,  344,  700,  701,  344,  304,  188,  314,  322,  479,  1,
    2,    3,    345,  1119, 210,  345,  346,  889,  211,  346,  969,  -480, -480, 970,  720,  347,
    722,  723,  347,  213,  215,  274,  286,  474,  730,  731,  732,  733,  734,  735,  736,  737,
    738,  739,  315,  288,  742,  348,  289,  349,  348,  350,  349,  352,  350,  295,  352,  353,
    475,  485,  353,  486,  354,  315,  759,  354,  338,  613,  355,  776,  615,  355,  356,  779,
    763,  356,  765,  753,  275,  276,  769,  619,  758,  277,  278,  189,  190,  191,  624,  625,
    192,  193,  194,  646,  203,  655,  218,  219,  656,  666,  668,  279,  280,  760,  220,  688,
    692,  794,  195,  196,  197,  281,  282,  266,  694,  696,  803,  804,  198,  199,  200,  704,
    272,  640,  643,  710,  711,  713,  714,  816,  719,  214,  653,  221,  222,  721,  658,  661,
    664,  761,  724,  725,  223,  224,  726,  727,  741,  683,  686,  837,  690,  225,  762,  841,
    764,  843,  766,  845,  767,  283,  768,  770,  771,  781,  707,  853,  772,  773,  673,  674,
    777,  859,  860,  228,  862,  863,  675,  778,  782,  783,  868,  784,  870,  785,  786,  873,
    874,  875,  876,  612,  787,  614,  229,  788,  789,  790,  791,  744,  747,  792,  793,  795,
    890,  796,  797,  676,  677,  798,  201,  202,  203,  204,  799,  800,  678,  679,  16,   17,
    18,   19,   20,   21,   801,  680,  207,  205,  305,  802,  805,  806,  909,  206,  807,  208,
    808,  810,  813,  912,  809,  811,  913,  695,  916,  917,  918,  681,  812,  919,  815,  817,
    920,  207,  205,  921,  818,  922,  304,  819,  206,  820,  208,  822,  823,  923,  924,  925,
    824,  825,  926,  826,  827,  927,  351,  351,  928,  828,  829,  930,  481,  997,  830,  931,
    357,  357,  775,  831,  364,  364,  1007, 1008, 1009, 344,  344,  833,  933,  832,  834,  934,
    935,  835,  836,  936,  937,  345,  345,  838,  939,  346,  346,  940,  839,  840,  941,  942,
    943,  842,  347,  347,  945,  844,  946,  947,  846,  948,  949,  814,  847,  848,  849,  950,
    850,  951,  851,  852,  952,  953,  954,  955,  348,  348,  349,  349,  350,  350,  352,  352,
    266,  855,  353,  353,  857,  957,  856,  354,  354,  958,  858,  894,  959,  355,  355,  960,
    861,  356,  356,  266,  864,  865,  266,  866,  867,  869,  871,  872,  877,  968,  878,  897,
    879,  880,  881,  882,  883,  884,  885,  984,  984,  886,  887,  888,  891,  892,  893,  992,
    895,  896,  898,  906,  907,  908,  911,  914,  929,  932,  938,  1005, 944,  956,  961,  962,
    1010, 592,  963,  1013, 1014, 1015, 964,  1017, 965,  966,  973,  975,  623,  982,  978,  990,
    991,  1021, 995,  999,  1024, 1025, 998,  1000, 1030, 1002, 1003, 647,  1004, 1035, 650,  651,
    1011, 980,  981,  1041, 983,  1012, 1016, 987,  988,  989,  1022, 670,  671,  1026, 993,  994,
    1018, 996,  1028, 1055, 693,  1057, 1058, 1036, 1037, 1038, 1006, 1039, 1056, 1040, 1077, 1042,
    1043, 709,  1044, 1045, 712,  1046, 715,  1019, 1020, 1076, 1047, 1079, 1065, 1078, 1048, 1081,
    1049, 1050, 1051, 1082, 1052, 1054, 1059, 1031, 1060, 1033, 1034, 1061, 472,  605,  605,  472,
    1066, 1101, 1068, 605,  605,  605,  1067, 605,  1069, 1070, 1102, 1100, 1071, 605,  605,  1072,
    1073, 1074, 1075, 1084, 1086, 1087, 605,  1088, 207,  205,  1089, 1090, 1091, 1092, 1093, 206,
    1114, 208,  1115, 1094, 1095, 1118, 604,  604,  1096, 1097, 1098, 1099, 604,  604,  604,  1116,
    604,  1103, 1104, 1105, 1106, 605,  604,  604,  1123, 1124, 1107, 1108, 634,  634,  634,  604,
    1109, 1110, 1111, 1112, 605,  605,  634,  1126, 1113, 1120, 634,  634,  634,  1121, 1122, 605,
    605,  1125, 970,  1134, 1129, 634,  634,  1130, 634,  1139, 1131, 1132, 1133, 1136, 604,  1137,
    1138, 1141, 1143, 1145, 1146, 901,  634,  1032, 752,  300,  1083, 977,  626,  604,  604,  629,
    630,  631,  632,  638,  641,  644,  756,  310,  604,  604,  290,  1135, 1142, 654,  1128, 1140,
    986,  659,  662,  665,  754,  593,  1127, 634,  634,  473,  672,  320,  684,  687,  0,    691,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    702,  703,  0,    705,  708,  0,    1144,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    266,  0,
    0,    0,    0,    0,    0,    0,    0,    902,  903,  904,  0,    740,  608,  608,  745,  748,
    0,    0,    608,  608,  608,  0,    608,  0,    0,    0,    0,    0,    608,  608,  0,    0,
    0,    0,    635,  635,  635,  608,  0,    266,  0,    0,    0,    0,    635,  0,    0,    0,
    635,  635,  635,  0,    0,    0,    0,    0,    0,    0,    0,    635,  635,  0,    635,  0,
    0,    0,    0,    0,    608,  0,    0,    0,    0,    0,    0,    0,    635,  0,    0,    0,
    0,    0,    0,    608,  608,  0,    0,    0,    0,    0,    0,    0,    0,    0,    608,  608,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    635,  635,  472,
    472,  73,   74,   75,   76,   77,   78,   79,   31,   32,   33,   34,   35,   0,    36,   37,
    38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,
    54,   55,   56,   80,   81,   82,   57,   83,   84,   0,    0,    85,   0,    86,   87,   88,
    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,   0,    0,    0,    99,   100,  0,
    0,    0,    0,    101,  102,  0,    103,  104,  0,    0,    105,  106,  107,  60,   108,  109,
    0,    0,    0,    0,    110,  111,  112,  113,  114,  115,  116,  0,    0,    0,    117,  118,
    119,  120,  121,  122,  123,  124,  125,  126,  0,    127,  128,  129,  130,  0,    0,    131,
    132,  133,  134,  135,  136,  137,  0,    0,    138,  139,  140,  141,  142,  143,  144,  0,
    145,  146,  147,  148,  149,  150,  151,  152,  153,  154,  0,    0,    155,  156,  157,  158,
    159,  160,  161,  162,  163,  0,    164,  165,  166,  167,  168,  169,  170,  171,  172,  173,
    174,  175,  176,  177,  178,  63,   179,  488,  489,  490,  491,  492,  493,  494,  31,   32,
    33,   34,   35,   0,    36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,
    48,   49,   50,   51,   52,   53,   54,   55,   56,   495,  496,  497,  57,   498,  499,  0,
    0,    500,  0,    501,  502,  503,  504,  505,  506,  507,  508,  509,  510,  511,  512,  513,
    0,    0,    0,    514,  515,  0,    0,    0,    0,    0,    516,  0,    517,  518,  0,    0,
    519,  520,  521,  522,  523,  524,  0,    0,    0,    0,    525,  526,  527,  528,  529,  530,
    531,  0,    0,    0,    532,  533,  534,  535,  536,  537,  538,  539,  540,  541,  0,    542,
    543,  544,  545,  0,    0,    546,  547,  548,  549,  550,  551,  552,  0,    0,    553,  554,
    555,  556,  557,  558,  559,  0,    560,  561,  562,  563,  0,    0,    0,    0,    0,    0,
    0,    0,    564,  565,  566,  567,  568,  569,  570,  571,  572,  0,    573,  574,  575,  576,
    577,  578,  579,  580,  581,  582,  583,  584,  585,  586,  587,  298,  299,  73,   74,   75,
    76,   77,   78,   79,   31,   32,   33,   34,   35,   0,    36,   37,   38,   39,   40,   41,
    42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,   80,
    81,   82,   57,   83,   84,   0,    0,    85,   0,    86,   87,   88,   89,   90,   91,   92,
    93,   94,   95,   96,   97,   98,   0,    0,    0,    99,   100,  0,    0,    0,    0,    309,
    102,  0,    103,  104,  0,    0,    105,  106,  107,  60,   108,  109,  0,    0,    0,    0,
    110,  111,  112,  113,  114,  115,  116,  0,    0,    0,    117,  118,  119,  120,  121,  122,
    123,  124,  125,  126,  0,    127,  128,  129,  130,  0,    0,    131,  132,  133,  134,  135,
    136,  137,  0,    0,    138,  139,  140,  141,  142,  143,  144,  0,    145,  146,  147,  148,
    149,  150,  151,  152,  153,  154,  0,    0,    155,  156,  157,  158,  159,  160,  161,  162,
    163,  0,    164,  165,  166,  167,  168,  169,  170,  171,  172,  173,  174,  175,  176,  177,
    178,  63,   488,  489,  490,  491,  492,  493,  494,  0,    0,    611,  0,    0,    0,    0,
    616,  617,  618,  0,    621,  0,    0,    0,    0,    0,    627,  628,  0,    0,    0,    0,
    0,    0,    0,    645,  495,  496,  497,  0,    498,  499,  0,    0,    500,  0,    501,  502,
    503,  504,  505,  506,  507,  508,  509,  510,  511,  512,  513,  0,    0,    0,    514,  515,
    0,    0,    697,  0,    0,    516,  0,    517,  518,  0,    0,    519,  520,  521,  0,    523,
    524,  717,  718,  0,    0,    525,  526,  527,  528,  529,  530,  531,  728,  729,  0,    532,
    533,  534,  535,  536,  537,  538,  539,  540,  541,  0,    542,  543,  544,  545,  0,    0,
    546,  547,  548,  549,  550,  551,  552,  0,    0,    553,  554,  555,  556,  557,  558,  559,
    0,    560,  561,  562,  563,  0,    0,    0,    0,    0,    0,    0,    0,    564,  565,  566,
    567,  568,  569,  570,  571,  572,  0,    573,  574,  575,  576,  577,  578,  579,  580,  581,
    582,  583,  584,  585,  586,  587,  30,   0,    31,   32,   33,   34,   35,   0,    36,   37,
    38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,
    54,   55,   56,   0,    0,    0,    57,   31,   32,   33,   34,   35,   58,   36,   37,   38,
    39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   0,    59,   0,    57,   0,    0,    0,    0,    0,    0,    0,    60,   0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    296,  0,    0,    0,    0,    0,    61,   0,    62,   0,    297,  31,   32,   33,
    34,   35,   0,    36,   37,   38,   39,   40,   41,   42,   43,   44,   45,   46,   47,   48,
    49,   50,   51,   52,   53,   54,   55,   56,   0,    0,    0,    57,   0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    636,  639,  642,  0,
    0,    0,    0,    0,    0,    63,   652,  0,    751,  0,    657,  660,  663,  0,    0,    0,
    0,    0,    522,  0,    0,    682,  685,  0,    689,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    298,  299,  706,  31,   32,   33,   34,   35,   0,    36,   37,   38,
    39,   40,   41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,
    55,   56,   0,    0,    0,    57,   0,    743,  746,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    216,  217,  0,    0,
    0,    0,    899,  0,    0,    0,    0,    0,    0,    0,    298,  299,  60,   189,  190,  191,
    0,    0,    192,  193,  194,  307,  0,    0,    0,    0,    0,    218,  219,  0,    0,    0,
    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,    0,    198,  199,
    200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    221,  222,  0,    149,  150,  151,  152,  153,  154,  223,  224,  0,    0,    0,    0,    0,
    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    226,  227,  0,
    0,    0,    0,    0,    63,   0,    0,    0,    228,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,  233,
    234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,  243,  244,  216,
    217,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    189,  190,  191,  0,    0,    192,  193,  194,  595,  0,    0,    0,    0,    0,    218,  219,
    0,    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,
    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,
    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    338,  596,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,
    231,  232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,
    243,  244,  216,  217,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    189,  190,  191,  0,    0,    192,  193,  194,  910,  0,    0,    0,    0,
    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,
    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,
    224,  0,    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    338,  596,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    229,  230,  231,  232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,
    203,  204,  242,  243,  244,  216,  217,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    189,  190,  191,  0,    0,    192,  193,  194,  963,  0,
    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,
    0,    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,
    0,    0,    223,  224,  0,    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    338,  596,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    229,  230,  231,  232,  233,  234,  235,  236,  237,  238,  239,  240,
    241,  201,  202,  203,  204,  242,  243,  244,  216,  217,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    189,  190,  191,  0,    0,    192,  193,
    194,  1053, 0,    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,
    195,  196,  197,  0,    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,
    0,    0,    0,    0,    0,    223,  224,  0,    0,    0,    0,    0,    0,    0,    225,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    338,  596,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,  233,  234,  235,  236,  237,
    238,  239,  240,  241,  201,  202,  203,  204,  242,  243,  244,  216,  217,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    189,  190,  191,  0,
    0,    192,  193,  194,  1062, 0,    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,
    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,    0,    198,  199,  200,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    221,
    222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,    0,    0,    0,    0,
    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    338,  596,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,  233,  234,
    235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,  243,  244,  216,  217,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    189,
    190,  191,  0,    0,    192,  193,  194,  1063, 0,    0,    0,    0,    0,    218,  219,  0,
    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,    0,
    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,    0,
    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    338,
    596,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,
    232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,  243,
    244,  216,  217,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    189,  190,  191,  0,    0,    192,  193,  194,  1064, 0,    0,    0,    0,    0,
    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,
    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,
    0,    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    338,  596,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    229,  230,  231,  232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,
    204,  242,  243,  244,  216,  217,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    189,  190,  191,  0,    0,    192,  193,  194,  0,    0,    0,
    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,
    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,
    0,    223,  224,  0,    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    226,  227,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    229,  230,  231,  232,  233,  234,  235,  236,  237,  238,  239,  240,  241,
    201,  202,  203,  204,  242,  243,  244,  216,  217,  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    189,  190,  191,  0,    0,    192,  193,  194,
    0,    0,    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,
    196,  197,  0,    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,
    0,    0,    0,    0,    223,  224,  0,    0,    0,    0,    0,    0,    0,    225,  0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    338,  596,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    228,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,  233,  234,  235,  236,  237,  238,
    239,  240,  241,  201,  202,  203,  204,  242,  243,  244,  216,  217,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    189,  190,  191,  0,    0,
    192,  193,  194,  0,    0,    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,
    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    221,  222,
    0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,    0,    0,    0,    0,    0,
    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    603,  596,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,  233,  234,  235,
    236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,  243,  244,  216,  217,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    189,  190,
    191,  0,    0,    192,  193,  194,  0,    0,    0,    0,    0,    0,    218,  219,  0,    0,
    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,    0,    0,    198,
    199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,    0,    0,
    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    603,  633,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  230,  231,  232,
    233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,  242,  243,  244,
    216,  217,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    189,  190,  191,  0,    0,    192,  193,  194,  0,    0,    0,    0,    0,    0,    218,
    219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,  0,    0,    0,    0,    0,
    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,
    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    271,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,  0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,
    230,  231,  232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  201,  202,  203,  204,
    242,  243,  244,  324,  325,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    326,  327,  328,  0,    0,    329,  330,  331,  0,    0,    0,    0,
    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  332,  333,  334,  0,    0,
    0,    0,    0,    0,    0,    335,  336,  337,  0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    221,  222,  0,    0,    0,    0,    0,    0,    0,
    223,  224,  0,    0,    0,    0,    0,    0,    0,    225,  0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    338,  339,  0,    0,    0,    0,    0,    0,    0,    0,    0,    228,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    229,  0,    0,    232,  233,  234,  235,  236,  237,  238,  239,  240,  241,  340,
    341,  342,  343,  242,  243,  244,  189,  190,  191,  0,    0,    192,  193,  194,  0,    0,
    0,    0,    0,    0,    218,  219,  0,    0,    0,    0,    0,    0,    220,  195,  196,  197,
    0,    0,    0,    0,    0,    0,    0,    198,  199,  200,  0,    0,    0,    0,    0,    0,
    189,  190,  191,  0,    0,    192,  193,  194,  755,  221,  222,  0,    0,    0,    218,  219,
    0,    0,    223,  224,  0,    0,    220,  195,  196,  197,  0,    225,  0,    0,    0,    0,
    0,    198,  199,  200,  0,    0,    480,  0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    228,  0,    221,  222,  0,    0,    0,    0,    0,    0,    0,    223,  224,  0,    0,
    0,    0,    0,    0,    229,  225,  0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    201,  202,  203,  204,  0,    0,    0,    0,    0,    0,    228,  0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    229,  0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    201,  202,  203,  204};

const short ParserGen::yycheck_[] = {
    14,  14,   14,   488,  291,  71,   287,  17,   17,   17,  20,   20,  20,  141, 17,   500, 17,
    20,  1034, 20,   624,  111,  69,   310,  71,   140,  313, 289,  65,  66,  67,  141,  517, 518,
    141, 297,  705,  0,    300,  43,   44,   107,  588,  589, 529,  297, 531, 141, 300,  297, 178,
    68,  300,  315,  297,  547,  548,  300,  339,  141,  107, 151,  314, 190, 191, 192,  297, 1083,
    141, 300,  297,  740,  141,  300,  21,   22,   23,   24,  563,  297, 565, 566, 300,  141, 69,
    140, 69,   69,   573,  574,  575,  576,  577,  578,  579, 580,  581, 582, 164, 34,   585, 297,
    11,  297,  300,  297,  300,  297,  300,  99,   300,  297, 30,   68,  300, 69,  297,  164, 603,
    300, 140,  140,  297,  19,   140,  300,  297,  14,   613, 300,  615, 69,  60,  61,   619, 140,
    69,  65,   66,   60,   61,   62,   140,  140,  65,   66,  67,   140, 185, 140, 74,   75,  140,
    140, 140,  83,   84,   69,   82,   140,  140,  646,  83,  84,   85,  93,  94,  58,   140, 140,
    655, 656,  93,   94,   95,   140,  67,   512,  513,  140, 140,  140, 140, 668, 140,  21,  521,
    111, 112,  140,  525,  526,  527,  69,   140,  140,  120, 121,  140, 140, 140, 536,  537, 688,
    539, 129,  69,   692,  69,   694,  69,   696,  69,   141, 69,   69,  69,  13,  553,  704, 69,
    69,  74,   75,   69,   710,  711,  151,  713,  714,  82,  69,   12,  12,  719, 69,   721, 69,
    12,  724,  725,  726,  727,  491,  69,   493,  170,  69,  12,   69,  69,  586, 587,  12,  69,
    69,  741,  69,   69,   111,  112,  69,   183,  184,  185, 186,  69,  69,  120, 121,  134, 135,
    136, 137,  138,  139,  69,   129,  288,  288,  288,  12,  69,   69,  769, 288, 12,   288, 69,
    12,  12,   776,  69,   69,   779,  543,  781,  782,  783, 151,  69,  786, 69,  69,   789, 315,
    315, 792,  69,   794,  591,  69,   315,  10,   315,  69,  69,   802, 803, 804, 69,   69,  807,
    69,  69,   810,  588,  589,  813,  69,   69,   816,  594, 937,  69,  820, 588, 589,  625, 69,
    588, 589,  946,  947,  948,  588,  589,  12,   833,  69,  69,   836, 837, 69,  12,   840, 841,
    588, 589,  69,   845,  588,  589,  848,  69,   12,   851, 852,  853, 69,  588, 589,  857, 69,
    859, 860,  69,   862,  863,  666,  69,   19,   69,   868, 69,   870, 16,  16,  873,  874, 875,
    876, 588,  589,  588,  589,  588,  589,  588,  589,  291, 69,   588, 589, 12,  890,  69,  588,
    589, 894,  69,   12,   897,  588,  589,  900,  69,   588, 589,  310, 69,  69,  313,  69,  69,
    69,  69,   69,   69,   914,  69,   12,   69,   69,   69,  69,   69,  69,  69,  924,  925, 69,
    69,  69,   69,   69,   69,   932,  69,   69,   69,   68,  68,   68,  68,  18,  68,   19,  68,
    944, 19,   19,   69,   69,   949,  475,  68,   952,  953, 954,  69,  956, 69,  27,   17,  20,
    501, 68,   35,   69,   68,   966,  68,   68,   969,  970, 69,   29,  973, 19,  19,   516, 68,
    978, 519,  520,  68,   920,  921,  984,  923,  68,   68,  926,  927, 928, 28,  532,  533, 25,
    933, 934,  68,   936,  22,   1000, 541,  1002, 1003, 69,  69,   69,  945, 69,  31,   69,  36,
    68,  68,   554,  69,   69,   557,  69,   559,  958,  959, 1022, 69,  26,  68,  1026, 69,  1028,
    69,  69,   69,   23,   69,   69,   69,   974,  69,   976, 977,  69,  297, 489, 490,  300, 69,
    32,  68,   495,  496,  497,  69,   499,  68,   68,   32,  1056, 69,  505, 506, 69,   69,  69,
    69,  69,   69,   69,   514,  69,   594,  594,  69,   69,  69,   69,  69,  594, 1077, 594, 1079,
    69,  69,   1082, 489,  490,  69,   69,   69,   69,   495, 496,  497, 33,  499, 69,   69,  69,
    69,  545,  505,  506,  1101, 1102, 69,   69,   511,  512, 513,  514, 69,  69,  69,   69,  560,
    561, 521,  1116, 69,   69,   525,  526,  527,  69,   69,  571,  572, 69,  24,  40,   69,  536,
    537, 69,   539,  1134, 69,   69,   69,   69,   545,  69,  69,   69,  69,  69,  69,   757, 553,
    975, 591,  287,  1031, 918,  504,  560,  561,  507,  508, 509,  510, 511, 512, 513,  594, 292,
    571, 572,  215,  1128, 1140, 521,  1118, 1135, 925,  525, 526,  527, 593, 478, 1117, 586, 587,
    300, 534,  294,  536,  537,  -1,   539,  -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  549,
    550, -1,   552,  553,  -1,   1142, -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   625,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  757, 757, 757,  -1,  583,
    489, 490,  586,  587,  -1,   -1,   495,  496,  497,  -1,  499,  -1,  -1,  -1,  -1,   -1,  505,
    506, -1,   -1,   -1,   -1,   511,  512,  513,  514,  -1,  666,  -1,  -1,  -1,  -1,   521, -1,
    -1,  -1,   525,  526,  527,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  536, 537,  -1,  539,
    -1,  -1,   -1,   -1,   -1,   545,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  553, -1,   -1,  -1,
    -1,  -1,   -1,   560,  561,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  571,  572, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  586, 587, 588,  589, 3,
    4,   5,    6,    7,    8,    9,    10,   11,   12,   13,  14,   -1,  16,  17,  18,   19,  20,
    21,  22,   23,   24,   25,   26,   27,   28,   29,   30,  31,   32,  33,  34,  35,   36,  37,
    38,  39,   40,   41,   42,   -1,   -1,   45,   -1,   47,  48,   49,  50,  51,  52,   53,  54,
    55,  56,   57,   58,   59,   -1,   -1,   -1,   63,   64,  -1,   -1,  -1,  -1,  69,   70,  -1,
    72,  73,   -1,   -1,   76,   77,   78,   79,   80,   81,  -1,   -1,  -1,  -1,  86,   87,  88,
    89,  90,   91,   92,   -1,   -1,   -1,   96,   97,   98,  99,   100, 101, 102, 103,  104, 105,
    -1,  107,  108,  109,  110,  -1,   -1,   113,  114,  115, 116,  117, 118, 119, -1,   -1,  122,
    123, 124,  125,  126,  127,  128,  -1,   130,  131,  132, 133,  134, 135, 136, 137,  138, 139,
    -1,  -1,   142,  143,  144,  145,  146,  147,  148,  149, 150,  -1,  152, 153, 154,  155, 156,
    157, 158,  159,  160,  161,  162,  163,  164,  165,  166, 167,  168, 3,   4,   5,    6,   7,
    8,   9,    10,   11,   12,   13,   14,   -1,   16,   17,  18,   19,  20,  21,  22,   23,  24,
    25,  26,   27,   28,   29,   30,   31,   32,   33,   34,  35,   36,  37,  38,  39,   40,  41,
    42,  -1,   -1,   45,   -1,   47,   48,   49,   50,   51,  52,   53,  54,  55,  56,   57,  58,
    59,  -1,   -1,   -1,   63,   64,   -1,   -1,   -1,   -1,  -1,   70,  -1,  72,  73,   -1,  -1,
    76,  77,   78,   79,   80,   81,   -1,   -1,   -1,   -1,  86,   87,  88,  89,  90,   91,  92,
    -1,  -1,   -1,   96,   97,   98,   99,   100,  101,  102, 103,  104, 105, -1,  107,  108, 109,
    110, -1,   -1,   113,  114,  115,  116,  117,  118,  119, -1,   -1,  122, 123, 124,  125, 126,
    127, 128,  -1,   130,  131,  132,  133,  -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   142, 143,
    144, 145,  146,  147,  148,  149,  150,  -1,   152,  153, 154,  155, 156, 157, 158,  159, 160,
    161, 162,  163,  164,  165,  166,  167,  168,  3,    4,   5,    6,   7,   8,   9,    10,  11,
    12,  13,   14,   -1,   16,   17,   18,   19,   20,   21,  22,   23,  24,  25,  26,   27,  28,
    29,  30,   31,   32,   33,   34,   35,   36,   37,   38,  39,   40,  41,  42,  -1,   -1,  45,
    -1,  47,   48,   49,   50,   51,   52,   53,   54,   55,  56,   57,  58,  59,  -1,   -1,  -1,
    63,  64,   -1,   -1,   -1,   -1,   69,   70,   -1,   72,  73,   -1,  -1,  76,  77,   78,  79,
    80,  81,   -1,   -1,   -1,   -1,   86,   87,   88,   89,  90,   91,  92,  -1,  -1,   -1,  96,
    97,  98,   99,   100,  101,  102,  103,  104,  105,  -1,  107,  108, 109, 110, -1,   -1,  113,
    114, 115,  116,  117,  118,  119,  -1,   -1,   122,  123, 124,  125, 126, 127, 128,  -1,  130,
    131, 132,  133,  134,  135,  136,  137,  138,  139,  -1,  -1,   142, 143, 144, 145,  146, 147,
    148, 149,  150,  -1,   152,  153,  154,  155,  156,  157, 158,  159, 160, 161, 162,  163, 164,
    165, 166,  167,  3,    4,    5,    6,    7,    8,    9,   -1,   -1,  490, -1,  -1,   -1,  -1,
    495, 496,  497,  -1,   499,  -1,   -1,   -1,   -1,   -1,  505,  506, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   514,  37,   38,   39,   -1,   41,   42,   -1,  -1,   45,  -1,  47,  48,   49,  50,
    51,  52,   53,   54,   55,   56,   57,   58,   59,   -1,  -1,   -1,  63,  64,  -1,   -1,  545,
    -1,  -1,   70,   -1,   72,   73,   -1,   -1,   76,   77,  78,   -1,  80,  81,  560,  561, -1,
    -1,  86,   87,   88,   89,   90,   91,   92,   571,  572, -1,   96,  97,  98,  99,   100, 101,
    102, 103,  104,  105,  -1,   107,  108,  109,  110,  -1,  -1,   113, 114, 115, 116,  117, 118,
    119, -1,   -1,   122,  123,  124,  125,  126,  127,  128, -1,   130, 131, 132, 133,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   142,  143,  144,  145, 146,  147, 148, 149, 150,  -1,  152,
    153, 154,  155,  156,  157,  158,  159,  160,  161,  162, 163,  164, 165, 166, 8,    -1,  10,
    11,  12,   13,   14,   -1,   16,   17,   18,   19,   20,  21,   22,  23,  24,  25,   26,  27,
    28,  29,   30,   31,   32,   33,   34,   35,   36,   -1,  -1,   -1,  40,  10,  11,   12,  13,
    14,  46,   16,   17,   18,   19,   20,   21,   22,   23,  24,   25,  26,  27,  28,   29,  30,
    31,  32,   33,   34,   35,   36,   -1,   69,   -1,   40,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    79,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   69,   -1,   -1,   -1,   -1,   -1,  106,  -1,  108, -1,  79,   10,  11,
    12,  13,   14,   -1,   16,   17,   18,   19,   20,   21,  22,   23,  24,  25,  26,   27,  28,
    29,  30,   31,   32,   33,   34,   35,   36,   -1,   -1,  -1,   40,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   511, 512, 513, -1,   -1,  -1,
    -1,  -1,   -1,   167,  521,  -1,   69,   -1,   525,  526, 527,  -1,  -1,  -1,  -1,   -1,  79,
    -1,  -1,   536,  537,  -1,   539,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    167, 168,  553,  10,   11,   12,   13,   14,   -1,   16,  17,   18,  19,  20,  21,   22,  23,
    24,  25,   26,   27,   28,   29,   30,   31,   32,   33,  34,   35,  36,  -1,  -1,   -1,  40,
    -1,  586,  587,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   43,   44,   -1,   -1,   -1,  -1,   69,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   167,  168,  79,   60,   61,   62,   -1,   -1,  65,   66,  67,  68,  -1,   -1,  -1,
    -1,  -1,   74,   75,   -1,   -1,   -1,   -1,   -1,   -1,  82,   83,  84,  85,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   93,   94,   95,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   111,  112,  -1,   134,  135, 136,  137, 138, 139, 120,  121, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   129,  -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    140, 141,  -1,   -1,   -1,   -1,   -1,   167,  -1,   -1,  -1,   151, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  170, 171,  172, 173,
    174, 175,  176,  177,  178,  179,  180,  181,  182,  183, 184,  185, 186, 187, 188,  189, 43,
    44,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  60,
    61,  62,   -1,   -1,   65,   66,   67,   68,   -1,   -1,  -1,   -1,  -1,  74,  75,   -1,  -1,
    -1,  -1,   -1,   -1,   82,   83,   84,   85,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   93,  94,
    95,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  111,
    112, -1,   -1,   -1,   -1,   -1,   -1,   -1,   120,  121, -1,   -1,  -1,  -1,  -1,   -1,  -1,
    129, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   140, 141, -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   151,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   170,  171,  172, 173,  174, 175, 176, 177,  178, 179,
    180, 181,  182,  183,  184,  185,  186,  187,  188,  189, 43,   44,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  60,   61,  62,  -1,  -1,   65,  66,
    67,  68,   -1,   -1,   -1,   -1,   -1,   74,   75,   -1,  -1,   -1,  -1,  -1,  -1,   82,  83,
    84,  85,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   93,  94,   95,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  111,  112, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   120,  121,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   129, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   140,  141,  -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  151,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  170,  171,  172,  173,  174,  175,  176,  177,  178, 179,  180, 181, 182, 183,  184, 185,
    186, 187,  188,  189,  43,   44,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   60,   61,   62,   -1,   -1,   65,  66,   67,  68,  -1,  -1,   -1,  -1,
    -1,  74,   75,   -1,   -1,   -1,   -1,   -1,   -1,   82,  83,   84,  85,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   93,   94,   95,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   111,  112,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  120, 121,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   129,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  140,
    141, -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  151,  -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  170, 171, 172,  173, 174,
    175, 176,  177,  178,  179,  180,  181,  182,  183,  184, 185,  186, 187, 188, 189,  43,  44,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   60,  61,
    62,  -1,   -1,   65,   66,   67,   68,   -1,   -1,   -1,  -1,   -1,  74,  75,  -1,   -1,  -1,
    -1,  -1,   -1,   82,   83,   84,   85,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  93,   94,  95,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   111, 112,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   120,  121,  -1,  -1,   -1,  -1,  -1,  -1,   -1,  129,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  140,  141, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   151,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   170,  171,  172,  173, 174,  175, 176, 177, 178,  179, 180,
    181, 182,  183,  184,  185,  186,  187,  188,  189,  43,  44,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,  61,   62,  -1,  -1,  65,   66,  67,
    68,  -1,   -1,   -1,   -1,   -1,   74,   75,   -1,   -1,  -1,   -1,  -1,  -1,  82,   83,  84,
    85,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   93,   94,  95,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   111, 112,  -1,  -1,  -1,  -1,   -1,  -1,
    -1,  120,  121,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  129,  -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   140,  141,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   151, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    170, 171,  172,  173,  174,  175,  176,  177,  178,  179, 180,  181, 182, 183, 184,  185, 186,
    187, 188,  189,  43,   44,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   60,   61,   62,   -1,   -1,   65,   66,  67,   68,  -1,  -1,  -1,   -1,  -1,
    74,  75,   -1,   -1,   -1,   -1,   -1,   -1,   82,   83,  84,   85,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   93,   94,   95,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   111,  112,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  120, 121, -1,   -1,  -1,
    -1,  -1,   -1,   -1,   129,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   140, 141,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   151, -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   170, 171, 172, 173,  174, 175,
    176, 177,  178,  179,  180,  181,  182,  183,  184,  185, 186,  187, 188, 189, 43,   44,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  60,   61,  62,
    -1,  -1,   65,   66,   67,   68,   -1,   -1,   -1,   -1,  -1,   74,  75,  -1,  -1,   -1,  -1,
    -1,  -1,   82,   83,   84,   85,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  93,  94,   95,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  111,  112, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   120,  121,  -1,   -1,  -1,   -1,  -1,  -1,  -1,   129, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   140, 141,  -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   151,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   170,  171,  172,  173,  174, 175,  176, 177, 178, 179,  180, 181,
    182, 183,  184,  185,  186,  187,  188,  189,  43,   44,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   60,   61,  62,   -1,  -1,  65,  66,   67,  -1,
    -1,  -1,   -1,   -1,   -1,   74,   75,   -1,   -1,   -1,  -1,   -1,  -1,  82,  83,   84,  85,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   93,   94,   95,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   111,  112, -1,   -1,  -1,  -1,  -1,   -1,  -1,
    120, 121,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   129, -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   140,  141,  -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  151,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  170,
    171, 172,  173,  174,  175,  176,  177,  178,  179,  180, 181,  182, 183, 184, 185,  186, 187,
    188, 189,  43,   44,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   60,   61,   62,   -1,   -1,   65,   66,   67,  -1,   -1,  -1,  -1,  -1,   -1,  74,
    75,  -1,   -1,   -1,   -1,   -1,   -1,   82,   83,   84,  85,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  93,   94,   95,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   111,  112,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   120, 121, -1,  -1,   -1,  -1,
    -1,  -1,   -1,   129,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  140,  141, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   151,  -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  170,  171, 172, 173, 174,  175, 176,
    177, 178,  179,  180,  181,  182,  183,  184,  185,  186, 187,  188, 189, 43,  44,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  60,  61,   62,  -1,
    -1,  65,   66,   67,   -1,   -1,   -1,   -1,   -1,   -1,  74,   75,  -1,  -1,  -1,   -1,  -1,
    -1,  82,   83,   84,   85,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  93,  94,  95,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  111, 112,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   120,  121,  -1,   -1,   -1,  -1,   -1,  -1,  -1,  129,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   140,  141, -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   151,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   170,  171,  172,  173,  174,  175, 176,  177, 178, 179, 180,  181, 182,
    183, 184,  185,  186,  187,  188,  189,  43,   44,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   60,   61,   62,  -1,   -1,  65,  66,  67,   -1,  -1,
    -1,  -1,   -1,   -1,   74,   75,   -1,   -1,   -1,   -1,  -1,   -1,  82,  83,  84,   85,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   93,   94,   95,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   111,  112,  -1,  -1,   -1,  -1,  -1,  -1,   -1,  120,
    121, -1,   -1,   -1,   -1,   -1,   -1,   -1,   129,  -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   140,  141,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  151, -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   170, 171,
    172, 173,  174,  175,  176,  177,  178,  179,  180,  181, 182,  183, 184, 185, 186,  187, 188,
    189, 43,   44,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  60,   61,   62,   -1,   -1,   65,   66,   67,   -1,  -1,   -1,  -1,  -1,  -1,   74,  75,
    -1,  -1,   -1,   -1,   -1,   -1,   82,   83,   84,   85,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    93,  94,   95,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  111,  112,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  120,  121, -1,  -1,  -1,   -1,  -1,
    -1,  -1,   129,  -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  141,  -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   151,  -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   170, 171,  172, 173, 174, 175,  176, 177,
    178, 179,  180,  181,  182,  183,  184,  185,  186,  187, 188,  189, 43,  44,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  60,  61,  62,   -1,  -1,
    65,  66,   67,   -1,   -1,   -1,   -1,   -1,   -1,   74,  75,   -1,  -1,  -1,  -1,   -1,  -1,
    82,  83,   84,   85,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   93,  94,  95,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  111, 112, -1,   -1,  -1,
    -1,  -1,   -1,   -1,   120,  121,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  129, -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   140,  141,  -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  151,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   170,  -1,   -1,   173,  174,  175,  176, 177,  178, 179, 180, 181,  182, 183,
    184, 185,  186,  187,  188,  189,  60,   61,   62,   -1,  -1,   65,  66,  67,  -1,   -1,  -1,
    -1,  -1,   -1,   74,   75,   -1,   -1,   -1,   -1,   -1,  -1,   82,  83,  84,  85,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   93,   94,   95,   -1,   -1,  -1,   -1,  -1,  -1,  60,   61,  62,
    -1,  -1,   65,   66,   67,   68,   111,  112,  -1,   -1,  -1,   74,  75,  -1,  -1,   120, 121,
    -1,  -1,   82,   83,   84,   85,   -1,   129,  -1,   -1,  -1,   -1,  -1,  93,  94,   95,  -1,
    -1,  140,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  151, -1,  111,  112, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   120,  121,  -1,   -1,  -1,   -1,  -1,  -1,  170,  129, -1,
    -1,  -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,   -1,  183,  184, 185, 186, -1,   -1,  -1,
    -1,  -1,   -1,   151,  -1,   -1,   -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  -1,   -1,   -1,   -1,   170,  -1,   -1,   -1,   -1,  -1,   -1,  -1,  -1,  -1,   -1,  -1,
    -1,  183,  184,  185,  186};

const short ParserGen::yystos_[] = {
    0,   190, 191, 192, 417, 140, 241, 141, 397, 141, 411, 0,   141, 242, 398, 412, 134, 135, 136,
    137, 138, 139, 243, 244, 245, 246, 247, 248, 249, 68,  8,   10,  11,  12,  13,  14,  16,  17,
    18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,
    40,  46,  69,  79,  106, 108, 167, 198, 201, 203, 206, 211, 402, 403, 408, 409, 3,   4,   5,
    6,   7,   8,   9,   37,  38,  39,  41,  42,  45,  47,  48,  49,  50,  51,  52,  53,  54,  55,
    56,  57,  58,  59,  63,  64,  69,  70,  72,  73,  76,  77,  78,  80,  81,  86,  87,  88,  89,
    90,  91,  92,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 107, 108, 109, 110, 113, 114,
    115, 116, 117, 118, 119, 122, 123, 124, 125, 126, 127, 128, 130, 131, 132, 133, 134, 135, 136,
    137, 138, 139, 142, 143, 144, 145, 146, 147, 148, 149, 150, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 166, 168, 197, 198, 200, 201, 202, 203, 205, 416, 141, 60,
    61,  62,  65,  66,  67,  83,  84,  85,  93,  94,  95,  183, 184, 185, 186, 216, 218, 219, 220,
    255, 141, 141, 255, 141, 418, 69,  43,  44,  74,  75,  82,  111, 112, 120, 121, 129, 140, 141,
    151, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 187, 188, 189, 212, 213,
    214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232,
    233, 234, 235, 236, 238, 141, 233, 400, 140, 60,  61,  65,  66,  83,  84,  93,  94,  141, 413,
    414, 69,  250, 34,  11,  242, 365, 237, 399, 401, 99,  69,  79,  167, 168, 194, 195, 199, 207,
    211, 255, 224, 68,  223, 69,  205, 210, 69,  71,  107, 164, 404, 405, 406, 407, 397, 111, 151,
    415, 43,  44,  60,  61,  62,  65,  66,  67,  83,  84,  85,  93,  94,  95,  140, 141, 183, 184,
    185, 186, 212, 213, 214, 215, 217, 221, 222, 224, 226, 227, 228, 230, 231, 232, 252, 253, 254,
    257, 260, 261, 262, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278, 279,
    280, 281, 282, 283, 284, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299,
    300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318,
    319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337,
    338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 367, 368, 369, 370, 371, 372, 373, 374,
    375, 376, 377, 378, 379, 380, 381, 382, 383, 384, 385, 386, 387, 388, 389, 390, 393, 252, 69,
    30,  223, 223, 141, 232, 140, 224, 239, 240, 255, 68,  69,  364, 3,   4,   5,   6,   7,   8,
    9,   37,  38,  39,  41,  42,  45,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,
    59,  63,  64,  70,  72,  73,  76,  77,  78,  79,  80,  81,  86,  87,  88,  89,  90,  91,  92,
    96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 107, 108, 109, 110, 113, 114, 115, 116, 117,
    118, 119, 122, 123, 124, 125, 126, 127, 128, 130, 131, 132, 133, 142, 143, 144, 145, 146, 147,
    148, 149, 150, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 194,
    204, 208, 251, 219, 399, 410, 68,  141, 233, 256, 262, 263, 393, 256, 140, 233, 263, 391, 392,
    393, 394, 395, 395, 262, 140, 262, 140, 395, 395, 395, 140, 258, 395, 256, 258, 140, 140, 418,
    395, 395, 418, 418, 418, 418, 141, 233, 393, 394, 396, 418, 394, 396, 418, 394, 396, 418, 395,
    140, 258, 256, 256, 258, 258, 394, 396, 418, 140, 140, 394, 396, 418, 394, 396, 418, 394, 396,
    418, 140, 256, 140, 256, 258, 258, 418, 74,  75,  82,  111, 112, 120, 121, 129, 151, 394, 396,
    418, 394, 396, 418, 140, 394, 396, 418, 140, 258, 140, 262, 140, 395, 285, 418, 285, 285, 418,
    418, 140, 418, 394, 396, 418, 258, 140, 140, 258, 140, 140, 258, 259, 395, 395, 140, 256, 140,
    256, 256, 140, 140, 140, 140, 395, 395, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 418,
    140, 256, 394, 396, 418, 394, 396, 418, 253, 253, 69,  208, 69,  404, 68,  240, 264, 69,  256,
    69,  69,  69,  256, 69,  256, 69,  69,  69,  256, 69,  69,  69,  69,  364, 223, 19,  69,  69,
    14,  359, 13,  12,  12,  69,  69,  12,  69,  69,  12,  69,  69,  12,  69,  256, 69,  69,  69,
    69,  69,  69,  69,  12,  256, 256, 69,  69,  12,  69,  69,  12,  69,  69,  12,  223, 69,  256,
    69,  69,  69,  10,  353, 69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  12,  69,  69,
    12,  256, 69,  69,  12,  256, 69,  256, 69,  256, 69,  69,  19,  69,  69,  16,  16,  256, 353,
    69,  69,  12,  69,  256, 256, 69,  256, 256, 69,  69,  69,  69,  256, 69,  256, 69,  69,  256,
    256, 256, 256, 69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  353, 256, 69,  69,
    69,  12,  69,  69,  12,  69,  69,  196, 197, 198, 201, 203, 209, 68,  68,  68,  256, 68,  68,
    256, 256, 18,  355, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 68,  256,
    256, 19,  256, 256, 256, 256, 256, 68,  256, 256, 256, 256, 256, 19,  256, 256, 256, 256, 256,
    256, 256, 256, 256, 256, 256, 19,  256, 256, 256, 256, 69,  69,  68,  69,  69,  27,  349, 256,
    21,  24,  358, 363, 17,  351, 20,  361, 351, 35,  352, 352, 352, 68,  352, 256, 366, 366, 352,
    352, 352, 69,  68,  256, 352, 352, 68,  352, 364, 69,  68,  29,  354, 19,  19,  68,  256, 352,
    364, 364, 364, 256, 68,  68,  256, 256, 256, 68,  256, 68,  352, 352, 256, 28,  350, 256, 256,
    25,  356, 22,  360, 256, 352, 229, 352, 352, 256, 69,  69,  69,  69,  69,  256, 68,  68,  69,
    69,  69,  69,  69,  69,  69,  69,  69,  68,  69,  256, 31,  256, 256, 69,  69,  69,  68,  68,
    68,  68,  69,  69,  68,  68,  68,  69,  69,  69,  69,  69,  256, 36,  256, 26,  362, 256, 23,
    349, 69,  350, 69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  256, 32,
    32,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  69,  256, 256, 33,  357, 256, 350, 69,
    69,  69,  256, 256, 69,  256, 352, 358, 69,  69,  69,  69,  69,  40,  356, 69,  69,  69,  256,
    362, 69,  357, 69,  352, 69,  69};

const short ParserGen::yyr1_[] = {
    0,   193, 417, 417, 417, 241, 242, 242, 418, 243, 243, 243, 243, 243, 243, 249, 244, 245, 255,
    255, 255, 255, 246, 247, 248, 250, 250, 207, 207, 252, 253, 253, 253, 253, 253, 253, 253, 253,
    253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253,
    253, 253, 253, 253, 253, 253, 253, 194, 195, 195, 195, 254, 251, 251, 208, 208, 397, 398, 398,
    402, 402, 402, 400, 400, 399, 399, 404, 404, 404, 406, 239, 410, 410, 240, 240, 407, 407, 408,
    405, 405, 403, 409, 409, 409, 401, 401, 206, 206, 206, 201, 197, 197, 197, 197, 197, 197, 198,
    199, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211, 211,
    211, 211, 211, 211, 211, 211, 211, 211, 211, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 224, 224, 224, 224, 224, 224,
    224, 224, 224, 224, 225, 238, 226, 227, 228, 230, 231, 232, 212, 213, 214, 215, 217, 221, 222,
    216, 216, 216, 216, 218, 218, 218, 218, 219, 219, 219, 219, 220, 220, 220, 220, 229, 229, 233,
    233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233, 233,
    233, 364, 364, 256, 256, 256, 256, 391, 391, 396, 396, 392, 392, 393, 393, 393, 393, 393, 393,
    393, 393, 393, 393, 393, 258, 259, 257, 257, 260, 261, 261, 262, 394, 395, 395, 263, 264, 264,
    209, 196, 196, 196, 196, 203, 204, 265, 265, 265, 265, 265, 265, 265, 265, 265, 265, 265, 265,
    265, 265, 265, 265, 266, 266, 266, 266, 266, 266, 266, 266, 266, 375, 375, 375, 375, 375, 375,
    375, 375, 375, 375, 375, 375, 375, 375, 375, 267, 388, 334, 335, 336, 337, 338, 339, 340, 341,
    342, 343, 344, 345, 346, 347, 348, 376, 377, 378, 379, 380, 381, 382, 383, 384, 385, 386, 387,
    389, 390, 268, 268, 268, 269, 270, 271, 275, 275, 275, 275, 275, 275, 275, 275, 275, 275, 275,
    275, 275, 275, 275, 275, 275, 275, 275, 275, 275, 275, 276, 351, 351, 352, 352, 277, 278, 307,
    307, 307, 307, 307, 307, 307, 307, 307, 307, 307, 307, 307, 307, 307, 355, 355, 356, 356, 357,
    357, 358, 358, 359, 359, 363, 363, 360, 360, 361, 361, 362, 362, 308, 308, 309, 310, 310, 310,
    311, 311, 311, 314, 314, 314, 312, 312, 312, 313, 313, 313, 319, 319, 319, 321, 321, 321, 315,
    315, 315, 316, 316, 316, 322, 322, 322, 320, 320, 320, 317, 317, 317, 318, 318, 318, 366, 366,
    366, 279, 280, 353, 353, 281, 288, 298, 354, 354, 285, 282, 283, 284, 286, 287, 289, 290, 291,
    292, 293, 294, 295, 296, 297, 415, 415, 413, 411, 412, 412, 414, 414, 414, 414, 414, 414, 414,
    414, 202, 202, 416, 416, 367, 367, 367, 367, 367, 367, 367, 368, 369, 370, 371, 372, 373, 374,
    272, 272, 273, 274, 223, 223, 234, 234, 235, 365, 365, 236, 237, 237, 210, 205, 205, 205, 205,
    205, 299, 299, 299, 299, 299, 299, 299, 300, 301, 302, 303, 304, 305, 306, 323, 323, 323, 323,
    323, 323, 323, 323, 323, 323, 349, 349, 350, 350, 324, 325, 326, 327, 328, 329, 330, 331, 332,
    333};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2,  2,  3, 0,  4,  1,  1,  1, 1, 1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4, 0, 2, 2, 2, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  3,  1,  2, 2, 2, 3, 0, 2, 2, 1, 1, 1, 3, 0, 2, 1, 1, 1, 2, 3, 0, 2,
    1, 1, 2, 2,  2,  2, 5,  5,  1,  1,  1, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  0, 2,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 5,
    1, 1, 1, 4,  4,  3, 3,  1,  1,  3,  0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1,  1,  4, 4,  4,  4,  4,  4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4,
    4, 4, 4, 7,  4,  4, 4,  7,  4,  7,  8, 7, 7, 4, 7, 7, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    1, 1, 1, 4,  4,  6, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 6, 0,
    2, 0, 2, 11, 10, 1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2,
    0, 2, 0, 2,  0,  2, 0,  2,  14, 16, 9, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 4,
    8, 4, 4, 8,  4,  4, 8,  4,  4,  8,  4, 4, 8, 4, 4, 8, 4, 4, 8, 4, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8,
    0, 2, 7, 4,  4,  4, 11, 11, 7,  4,  4, 7, 8, 8, 8, 4, 4, 1, 1, 4, 3, 0, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 2,  2,  1, 1,  1,  1,  1,  1, 1, 6, 6, 4, 8, 8, 4, 8, 1, 1, 6, 6, 1, 1, 1, 1, 3, 0, 2,
    3, 0, 2, 2,  1,  1, 1,  1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 0,  2,  0, 2,  11, 4,  4,  4, 4, 4, 4, 4, 4, 4};


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
                                           "nonArrayNonObjCompoundExpression",
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
    0,    385,  385,  388,  391,  398,  404,  405,  413,  416,  416,  416,  416,  416,  416,  419,
    429,  435,  445,  445,  445,  445,  449,  454,  459,  478,  481,  488,  491,  497,  511,  512,
    513,  514,  515,  516,  517,  518,  519,  520,  521,  522,  525,  528,  531,  534,  537,  540,
    543,  546,  549,  552,  555,  558,  561,  564,  567,  570,  573,  576,  577,  578,  579,  580,
    585,  593,  606,  607,  624,  631,  635,  643,  646,  652,  658,  661,  667,  670,  671,  678,
    679,  685,  688,  696,  696,  696,  700,  706,  712,  713,  720,  720,  724,  733,  743,  749,
    754,  764,  772,  773,  774,  777,  780,  787,  787,  787,  790,  798,  801,  804,  807,  810,
    813,  819,  825,  844,  847,  850,  853,  856,  859,  862,  865,  868,  871,  874,  877,  880,
    883,  886,  889,  892,  895,  898,  901,  904,  907,  910,  913,  916,  919,  922,  930,  933,
    936,  939,  942,  945,  948,  951,  954,  957,  960,  963,  966,  969,  972,  975,  978,  981,
    984,  987,  990,  993,  996,  999,  1002, 1005, 1008, 1011, 1014, 1017, 1020, 1023, 1026, 1029,
    1032, 1035, 1038, 1041, 1044, 1047, 1050, 1053, 1056, 1059, 1062, 1065, 1068, 1071, 1074, 1077,
    1080, 1083, 1086, 1089, 1092, 1095, 1098, 1101, 1104, 1107, 1110, 1113, 1116, 1119, 1122, 1125,
    1128, 1131, 1134, 1137, 1140, 1143, 1146, 1149, 1152, 1155, 1158, 1161, 1164, 1167, 1170, 1173,
    1176, 1179, 1182, 1185, 1188, 1191, 1194, 1197, 1200, 1203, 1206, 1209, 1212, 1215, 1218, 1221,
    1224, 1231, 1236, 1239, 1242, 1245, 1248, 1251, 1254, 1257, 1260, 1266, 1280, 1294, 1300, 1306,
    1312, 1318, 1324, 1330, 1336, 1342, 1348, 1354, 1360, 1366, 1372, 1375, 1378, 1381, 1387, 1390,
    1393, 1396, 1402, 1405, 1408, 1411, 1417, 1420, 1423, 1426, 1432, 1435, 1441, 1442, 1443, 1444,
    1445, 1446, 1447, 1448, 1449, 1450, 1451, 1452, 1453, 1454, 1455, 1456, 1457, 1458, 1459, 1460,
    1461, 1468, 1469, 1476, 1476, 1476, 1476, 1480, 1480, 1484, 1484, 1488, 1488, 1492, 1492, 1492,
    1492, 1492, 1492, 1492, 1493, 1493, 1493, 1493, 1498, 1505, 1511, 1511, 1514, 1518, 1522, 1531,
    1538, 1543, 1543, 1548, 1554, 1557, 1564, 1571, 1571, 1571, 1571, 1575, 1581, 1587, 1587, 1587,
    1587, 1587, 1587, 1587, 1587, 1587, 1587, 1587, 1587, 1588, 1588, 1588, 1588, 1592, 1595, 1598,
    1601, 1604, 1607, 1610, 1613, 1616, 1621, 1621, 1621, 1621, 1621, 1621, 1621, 1621, 1621, 1621,
    1621, 1621, 1621, 1622, 1622, 1626, 1633, 1639, 1644, 1649, 1655, 1660, 1665, 1670, 1676, 1681,
    1687, 1696, 1702, 1708, 1713, 1719, 1725, 1730, 1735, 1740, 1745, 1750, 1755, 1760, 1765, 1770,
    1775, 1780, 1785, 1790, 1796, 1796, 1796, 1800, 1807, 1814, 1821, 1821, 1821, 1821, 1821, 1821,
    1821, 1822, 1822, 1822, 1822, 1822, 1822, 1822, 1822, 1823, 1823, 1823, 1823, 1823, 1823, 1823,
    1827, 1837, 1840, 1846, 1849, 1856, 1865, 1874, 1874, 1874, 1874, 1874, 1874, 1874, 1874, 1874,
    1875, 1875, 1875, 1875, 1875, 1875, 1879, 1882, 1888, 1891, 1897, 1900, 1906, 1909, 1915, 1918,
    1924, 1927, 1933, 1936, 1942, 1945, 1951, 1954, 1960, 1966, 1975, 1983, 1986, 1990, 1996, 2000,
    2004, 2010, 2014, 2018, 2024, 2028, 2032, 2038, 2042, 2046, 2052, 2056, 2060, 2066, 2070, 2074,
    2080, 2084, 2088, 2094, 2098, 2102, 2108, 2112, 2116, 2122, 2126, 2130, 2136, 2140, 2144, 2150,
    2154, 2158, 2164, 2167, 2170, 2176, 2187, 2198, 2201, 2207, 2215, 2223, 2231, 2234, 2239, 2248,
    2254, 2260, 2266, 2276, 2286, 2293, 2300, 2307, 2315, 2323, 2331, 2339, 2345, 2351, 2354, 2360,
    2366, 2371, 2374, 2381, 2384, 2387, 2390, 2393, 2396, 2399, 2402, 2407, 2409, 2419, 2421, 2427,
    2427, 2427, 2427, 2427, 2427, 2428, 2432, 2438, 2444, 2451, 2462, 2473, 2480, 2491, 2491, 2495,
    2502, 2509, 2509, 2513, 2513, 2517, 2523, 2524, 2531, 2537, 2540, 2547, 2554, 2555, 2556, 2557,
    2558, 2561, 2561, 2561, 2561, 2561, 2561, 2561, 2563, 2568, 2573, 2578, 2583, 2588, 2593, 2599,
    2600, 2601, 2602, 2603, 2604, 2605, 2606, 2607, 2608, 2613, 2616, 2623, 2626, 2632, 2642, 2647,
    2652, 2657, 2662, 2667, 2672, 2677, 2682};

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
#line 9031 "parser_gen.cpp"

#line 2686 "grammar.yy"
