// A Bison parser, made by GNU Bison 3.7.

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
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
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
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
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
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
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
        case symbol_kind::S_predicate:              // predicate
        case symbol_kind::S_logicalExpr:            // logicalExpr
        case symbol_kind::S_operatorExpression:     // operatorExpression
        case symbol_kind::S_notExpr:                // notExpr
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
                case symbol_kind::S_predicate:              // predicate
                case symbol_kind::S_logicalExpr:            // logicalExpr
                case symbol_kind::S_operatorExpression:     // operatorExpression
                case symbol_kind::S_notExpr:                // notExpr
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
#line 349 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1978 "parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH match
#line 352 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1986 "parser_gen.cpp"
                    break;

                    case 4:  // start: START_SORT sortSpecs
#line 355 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1994 "parser_gen.cpp"
                    break;

                    case 5:  // pipeline: "array" stageList "end of array"
#line 362 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2002 "parser_gen.cpp"
                    break;

                    case 6:  // stageList: %empty
#line 368 "grammar.yy"
                    {
                    }
#line 2008 "parser_gen.cpp"
                    break;

                    case 7:  // stageList: "object" stage "end of object" stageList
#line 369 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2016 "parser_gen.cpp"
                    break;

                    case 8:  // $@1: %empty
#line 377 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2022 "parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2028 "parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2034 "parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2040 "parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2046 "parser_gen.cpp"
                    break;

                    case 14:  // stage: project
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2052 "parser_gen.cpp"
                    break;

                    case 15:  // stage: sample
#line 380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2058 "parser_gen.cpp"
                    break;

                    case 16:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 383 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2070 "parser_gen.cpp"
                    break;

                    case 17:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 393 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2078 "parser_gen.cpp"
                    break;

                    case 18:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 399 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2091 "parser_gen.cpp"
                    break;

                    case 19:  // num: int
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2097 "parser_gen.cpp"
                    break;

                    case 20:  // num: long
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2103 "parser_gen.cpp"
                    break;

                    case 21:  // num: double
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2109 "parser_gen.cpp"
                    break;

                    case 22:  // num: decimal
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2115 "parser_gen.cpp"
                    break;

                    case 23:  // skip: STAGE_SKIP num
#line 413 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2123 "parser_gen.cpp"
                    break;

                    case 24:  // limit: STAGE_LIMIT num
#line 418 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2131 "parser_gen.cpp"
                    break;

                    case 25:  // project: STAGE_PROJECT "object" projectFields "end of object"
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
#line 2152 "parser_gen.cpp"
                    break;

                    case 26:  // projectFields: %empty
#line 442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2160 "parser_gen.cpp"
                    break;

                    case 27:  // projectFields: projectFields projectField
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2169 "parser_gen.cpp"
                    break;

                    case 28:  // projectField: ID topLevelProjection
#line 452 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2177 "parser_gen.cpp"
                    break;

                    case 29:  // projectField: aggregationProjectionFieldname topLevelProjection
#line 455 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2185 "parser_gen.cpp"
                    break;

                    case 30:  // topLevelProjection: projection
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
#line 2201 "parser_gen.cpp"
                    break;

                    case 31:  // projection: string
#line 475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2207 "parser_gen.cpp"
                    break;

                    case 32:  // projection: binary
#line 476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2213 "parser_gen.cpp"
                    break;

                    case 33:  // projection: undefined
#line 477 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2219 "parser_gen.cpp"
                    break;

                    case 34:  // projection: objectId
#line 478 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2225 "parser_gen.cpp"
                    break;

                    case 35:  // projection: date
#line 479 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2231 "parser_gen.cpp"
                    break;

                    case 36:  // projection: null
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2237 "parser_gen.cpp"
                    break;

                    case 37:  // projection: regex
#line 481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2243 "parser_gen.cpp"
                    break;

                    case 38:  // projection: dbPointer
#line 482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2249 "parser_gen.cpp"
                    break;

                    case 39:  // projection: javascript
#line 483 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2255 "parser_gen.cpp"
                    break;

                    case 40:  // projection: symbol
#line 484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2261 "parser_gen.cpp"
                    break;

                    case 41:  // projection: javascriptWScope
#line 485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2267 "parser_gen.cpp"
                    break;

                    case 42:  // projection: "1 (int)"
#line 486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2275 "parser_gen.cpp"
                    break;

                    case 43:  // projection: "-1 (int)"
#line 489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2283 "parser_gen.cpp"
                    break;

                    case 44:  // projection: "arbitrary integer"
#line 492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2291 "parser_gen.cpp"
                    break;

                    case 45:  // projection: "zero (int)"
#line 495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2299 "parser_gen.cpp"
                    break;

                    case 46:  // projection: "1 (long)"
#line 498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2307 "parser_gen.cpp"
                    break;

                    case 47:  // projection: "-1 (long)"
#line 501 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2315 "parser_gen.cpp"
                    break;

                    case 48:  // projection: "arbitrary long"
#line 504 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2323 "parser_gen.cpp"
                    break;

                    case 49:  // projection: "zero (long)"
#line 507 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2331 "parser_gen.cpp"
                    break;

                    case 50:  // projection: "1 (double)"
#line 510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2339 "parser_gen.cpp"
                    break;

                    case 51:  // projection: "-1 (double)"
#line 513 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2347 "parser_gen.cpp"
                    break;

                    case 52:  // projection: "arbitrary double"
#line 516 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2355 "parser_gen.cpp"
                    break;

                    case 53:  // projection: "zero (double)"
#line 519 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2363 "parser_gen.cpp"
                    break;

                    case 54:  // projection: "1 (decimal)"
#line 522 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2371 "parser_gen.cpp"
                    break;

                    case 55:  // projection: "-1 (decimal)"
#line 525 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2379 "parser_gen.cpp"
                    break;

                    case 56:  // projection: "arbitrary decimal"
#line 528 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2387 "parser_gen.cpp"
                    break;

                    case 57:  // projection: "zero (decimal)"
#line 531 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2395 "parser_gen.cpp"
                    break;

                    case 58:  // projection: "true"
#line 534 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2403 "parser_gen.cpp"
                    break;

                    case 59:  // projection: "false"
#line 537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2411 "parser_gen.cpp"
                    break;

                    case 60:  // projection: timestamp
#line 540 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2417 "parser_gen.cpp"
                    break;

                    case 61:  // projection: minKey
#line 541 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2423 "parser_gen.cpp"
                    break;

                    case 62:  // projection: maxKey
#line 542 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2429 "parser_gen.cpp"
                    break;

                    case 63:  // projection: projectionObject
#line 543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2435 "parser_gen.cpp"
                    break;

                    case 64:  // projection: compoundNonObjectExpression
#line 544 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2441 "parser_gen.cpp"
                    break;

                    case 65:  // aggregationProjectionFieldname: projectionFieldname
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
#line 2451 "parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: "fieldname"
#line 557 "grammar.yy"
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
#line 2469 "parser_gen.cpp"
                    break;

                    case 67:  // projectionFieldname: argAsProjectionPath
#line 570 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2475 "parser_gen.cpp"
                    break;

                    case 68:  // projectionFieldname: "fieldname containing dotted path"
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
#line 2493 "parser_gen.cpp"
                    break;

                    case 69:  // projectionObject: "object" projectionObjectFields "end of object"
#line 588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2501 "parser_gen.cpp"
                    break;

                    case 70:  // projectionObjectFields: projectionObjectField
#line 595 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2510 "parser_gen.cpp"
                    break;

                    case 71:  // projectionObjectFields: projectionObjectFields
                              // projectionObjectField
#line 599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2519 "parser_gen.cpp"
                    break;

                    case 72:  // projectionObjectField: idAsProjectionPath projection
#line 607 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2527 "parser_gen.cpp"
                    break;

                    case 73:  // projectionObjectField: aggregationProjectionFieldname projection
#line 610 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2535 "parser_gen.cpp"
                    break;

                    case 74:  // match: "object" predicates "end of object"
#line 616 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2543 "parser_gen.cpp"
                    break;

                    case 75:  // predicates: %empty
#line 622 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2551 "parser_gen.cpp"
                    break;

                    case 76:  // predicates: predicates predicate
#line 625 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2560 "parser_gen.cpp"
                    break;

                    case 77:  // predicate: predFieldname predValue
#line 631 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2568 "parser_gen.cpp"
                    break;

                    case 78:  // predicate: logicalExpr
#line 634 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2576 "parser_gen.cpp"
                    break;

                    case 79:  // predValue: simpleValue
#line 643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2582 "parser_gen.cpp"
                    break;

                    case 80:  // predValue: "object" compoundMatchExprs "end of object"
#line 644 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2590 "parser_gen.cpp"
                    break;

                    case 81:  // compoundMatchExprs: %empty
#line 650 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2598 "parser_gen.cpp"
                    break;

                    case 82:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 653 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2607 "parser_gen.cpp"
                    break;

                    case 83:  // operatorExpression: notExpr
#line 660 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2613 "parser_gen.cpp"
                    break;

                    case 84:  // notExpr: NOT regex
#line 663 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2621 "parser_gen.cpp"
                    break;

                    case 85:  // notExpr: NOT "object" compoundMatchExprs operatorExpression "end of
                              // object"
#line 668 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2632 "parser_gen.cpp"
                    break;

                    case 86:  // logicalExpr: logicalExprField "array" additionalExprs match "end of
                              // array"
#line 678 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2642 "parser_gen.cpp"
                    break;

                    case 87:  // logicalExprField: AND
#line 686 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2648 "parser_gen.cpp"
                    break;

                    case 88:  // logicalExprField: OR
#line 687 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2654 "parser_gen.cpp"
                    break;

                    case 89:  // logicalExprField: NOR
#line 688 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2660 "parser_gen.cpp"
                    break;

                    case 90:  // additionalExprs: %empty
#line 691 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2668 "parser_gen.cpp"
                    break;

                    case 91:  // additionalExprs: additionalExprs match
#line 694 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2677 "parser_gen.cpp"
                    break;

                    case 92:  // predFieldname: idAsUserFieldname
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2683 "parser_gen.cpp"
                    break;

                    case 93:  // predFieldname: argAsUserFieldname
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2689 "parser_gen.cpp"
                    break;

                    case 94:  // predFieldname: invariableUserFieldname
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2695 "parser_gen.cpp"
                    break;

                    case 95:  // invariableUserFieldname: "fieldname"
#line 704 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2703 "parser_gen.cpp"
                    break;

                    case 96:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 712 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2711 "parser_gen.cpp"
                    break;

                    case 97:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 715 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2719 "parser_gen.cpp"
                    break;

                    case 98:  // stageAsUserFieldname: STAGE_SKIP
#line 718 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2727 "parser_gen.cpp"
                    break;

                    case 99:  // stageAsUserFieldname: STAGE_LIMIT
#line 721 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2735 "parser_gen.cpp"
                    break;

                    case 100:  // stageAsUserFieldname: STAGE_PROJECT
#line 724 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2743 "parser_gen.cpp"
                    break;

                    case 101:  // stageAsUserFieldname: STAGE_SAMPLE
#line 727 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2751 "parser_gen.cpp"
                    break;

                    case 102:  // argAsUserFieldname: arg
#line 733 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2759 "parser_gen.cpp"
                    break;

                    case 103:  // argAsProjectionPath: arg
#line 739 "grammar.yy"
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
#line 2777 "parser_gen.cpp"
                    break;

                    case 104:  // arg: "coll argument"
#line 758 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 2785 "parser_gen.cpp"
                    break;

                    case 105:  // arg: "pipeline argument"
#line 761 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 2793 "parser_gen.cpp"
                    break;

                    case 106:  // arg: "size argument"
#line 764 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 2801 "parser_gen.cpp"
                    break;

                    case 107:  // arg: "input argument"
#line 767 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 2809 "parser_gen.cpp"
                    break;

                    case 108:  // arg: "to argument"
#line 770 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 2817 "parser_gen.cpp"
                    break;

                    case 109:  // arg: "onError argument"
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 2825 "parser_gen.cpp"
                    break;

                    case 110:  // arg: "onNull argument"
#line 776 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 2833 "parser_gen.cpp"
                    break;

                    case 111:  // arg: "dateString argument"
#line 779 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 2841 "parser_gen.cpp"
                    break;

                    case 112:  // arg: "format argument"
#line 782 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 2849 "parser_gen.cpp"
                    break;

                    case 113:  // arg: "timezone argument"
#line 785 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 2857 "parser_gen.cpp"
                    break;

                    case 114:  // arg: "date argument"
#line 788 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 2865 "parser_gen.cpp"
                    break;

                    case 115:  // arg: "chars argument"
#line 791 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 2873 "parser_gen.cpp"
                    break;

                    case 116:  // arg: "regex argument"
#line 794 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 2881 "parser_gen.cpp"
                    break;

                    case 117:  // arg: "options argument"
#line 797 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 2889 "parser_gen.cpp"
                    break;

                    case 118:  // arg: "find argument"
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 2897 "parser_gen.cpp"
                    break;

                    case 119:  // arg: "replacement argument"
#line 803 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 2905 "parser_gen.cpp"
                    break;

                    case 120:  // aggExprAsUserFieldname: ADD
#line 811 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2913 "parser_gen.cpp"
                    break;

                    case 121:  // aggExprAsUserFieldname: ATAN2
#line 814 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2921 "parser_gen.cpp"
                    break;

                    case 122:  // aggExprAsUserFieldname: AND
#line 817 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2929 "parser_gen.cpp"
                    break;

                    case 123:  // aggExprAsUserFieldname: CONST_EXPR
#line 820 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2937 "parser_gen.cpp"
                    break;

                    case 124:  // aggExprAsUserFieldname: LITERAL
#line 823 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2945 "parser_gen.cpp"
                    break;

                    case 125:  // aggExprAsUserFieldname: OR
#line 826 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2953 "parser_gen.cpp"
                    break;

                    case 126:  // aggExprAsUserFieldname: NOT
#line 829 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2961 "parser_gen.cpp"
                    break;

                    case 127:  // aggExprAsUserFieldname: CMP
#line 832 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2969 "parser_gen.cpp"
                    break;

                    case 128:  // aggExprAsUserFieldname: EQ
#line 835 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2977 "parser_gen.cpp"
                    break;

                    case 129:  // aggExprAsUserFieldname: GT
#line 838 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2985 "parser_gen.cpp"
                    break;

                    case 130:  // aggExprAsUserFieldname: GTE
#line 841 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2993 "parser_gen.cpp"
                    break;

                    case 131:  // aggExprAsUserFieldname: LT
#line 844 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3001 "parser_gen.cpp"
                    break;

                    case 132:  // aggExprAsUserFieldname: LTE
#line 847 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3009 "parser_gen.cpp"
                    break;

                    case 133:  // aggExprAsUserFieldname: NE
#line 850 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3017 "parser_gen.cpp"
                    break;

                    case 134:  // aggExprAsUserFieldname: CONVERT
#line 853 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3025 "parser_gen.cpp"
                    break;

                    case 135:  // aggExprAsUserFieldname: TO_BOOL
#line 856 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3033 "parser_gen.cpp"
                    break;

                    case 136:  // aggExprAsUserFieldname: TO_DATE
#line 859 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3041 "parser_gen.cpp"
                    break;

                    case 137:  // aggExprAsUserFieldname: TO_DECIMAL
#line 862 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3049 "parser_gen.cpp"
                    break;

                    case 138:  // aggExprAsUserFieldname: TO_DOUBLE
#line 865 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3057 "parser_gen.cpp"
                    break;

                    case 139:  // aggExprAsUserFieldname: TO_INT
#line 868 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3065 "parser_gen.cpp"
                    break;

                    case 140:  // aggExprAsUserFieldname: TO_LONG
#line 871 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3073 "parser_gen.cpp"
                    break;

                    case 141:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 874 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3081 "parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: TO_STRING
#line 877 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3089 "parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: TYPE
#line 880 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3097 "parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: ABS
#line 883 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3105 "parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: CEIL
#line 886 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3113 "parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: DIVIDE
#line 889 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3121 "parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: EXPONENT
#line 892 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3129 "parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: FLOOR
#line 895 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3137 "parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: LN
#line 898 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3145 "parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: LOG
#line 901 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3153 "parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: LOGTEN
#line 904 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3161 "parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: MOD
#line 907 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3169 "parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: MULTIPLY
#line 910 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3177 "parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: POW
#line 913 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3185 "parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: ROUND
#line 916 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3193 "parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: "slice"
#line 919 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3201 "parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: SQRT
#line 922 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3209 "parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: SUBTRACT
#line 925 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3217 "parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: TRUNC
#line 928 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3225 "parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: CONCAT
#line 931 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3233 "parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 934 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3241 "parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 937 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3249 "parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 940 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3257 "parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 943 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3265 "parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: LTRIM
#line 946 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3273 "parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: META
#line 949 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3281 "parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: REGEX_FIND
#line 952 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3289 "parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 955 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3297 "parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: REGEX_MATCH
#line 958 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3305 "parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: REPLACE_ONE
#line 961 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3313 "parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: REPLACE_ALL
#line 964 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3321 "parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: RTRIM
#line 967 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3329 "parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: SPLIT
#line 970 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3337 "parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 973 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3345 "parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: STR_LEN_CP
#line 976 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3353 "parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 979 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3361 "parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: SUBSTR
#line 982 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3369 "parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 985 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3377 "parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: SUBSTR_CP
#line 988 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3385 "parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: TO_LOWER
#line 991 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3393 "parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: TRIM
#line 994 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3401 "parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: TO_UPPER
#line 997 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3409 "parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: "allElementsTrue"
#line 1000 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3417 "parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: "anyElementTrue"
#line 1003 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3425 "parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: "setDifference"
#line 1006 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3433 "parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: "setEquals"
#line 1009 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3441 "parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: "setIntersection"
#line 1012 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3449 "parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: "setIsSubset"
#line 1015 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3457 "parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: "setUnion"
#line 1018 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3465 "parser_gen.cpp"
                    break;

                    case 190:  // aggExprAsUserFieldname: SIN
#line 1021 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 3473 "parser_gen.cpp"
                    break;

                    case 191:  // aggExprAsUserFieldname: COS
#line 1024 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 3481 "parser_gen.cpp"
                    break;

                    case 192:  // aggExprAsUserFieldname: TAN
#line 1027 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 3489 "parser_gen.cpp"
                    break;

                    case 193:  // aggExprAsUserFieldname: SINH
#line 1030 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 3497 "parser_gen.cpp"
                    break;

                    case 194:  // aggExprAsUserFieldname: COSH
#line 1033 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 3505 "parser_gen.cpp"
                    break;

                    case 195:  // aggExprAsUserFieldname: TANH
#line 1036 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 3513 "parser_gen.cpp"
                    break;

                    case 196:  // aggExprAsUserFieldname: ASIN
#line 1039 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 3521 "parser_gen.cpp"
                    break;

                    case 197:  // aggExprAsUserFieldname: ACOS
#line 1042 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 3529 "parser_gen.cpp"
                    break;

                    case 198:  // aggExprAsUserFieldname: ATAN
#line 1045 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 3537 "parser_gen.cpp"
                    break;

                    case 199:  // aggExprAsUserFieldname: ASINH
#line 1048 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 3545 "parser_gen.cpp"
                    break;

                    case 200:  // aggExprAsUserFieldname: ACOSH
#line 1051 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 3553 "parser_gen.cpp"
                    break;

                    case 201:  // aggExprAsUserFieldname: ATANH
#line 1054 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 3561 "parser_gen.cpp"
                    break;

                    case 202:  // aggExprAsUserFieldname: DEGREES_TO_RADIANS
#line 1057 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 3569 "parser_gen.cpp"
                    break;

                    case 203:  // aggExprAsUserFieldname: RADIANS_TO_DEGREES
#line 1060 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 3577 "parser_gen.cpp"
                    break;

                    case 204:  // string: "string"
#line 1067 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3585 "parser_gen.cpp"
                    break;

                    case 205:  // string: "geoNearDistance"
#line 1072 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 3593 "parser_gen.cpp"
                    break;

                    case 206:  // string: "geoNearPoint"
#line 1075 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 3601 "parser_gen.cpp"
                    break;

                    case 207:  // string: "indexKey"
#line 1078 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 3609 "parser_gen.cpp"
                    break;

                    case 208:  // string: "randVal"
#line 1081 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3617 "parser_gen.cpp"
                    break;

                    case 209:  // string: "recordId"
#line 1084 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 3625 "parser_gen.cpp"
                    break;

                    case 210:  // string: "searchHighlights"
#line 1087 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 3633 "parser_gen.cpp"
                    break;

                    case 211:  // string: "searchScore"
#line 1090 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 3641 "parser_gen.cpp"
                    break;

                    case 212:  // string: "sortKey"
#line 1093 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 3649 "parser_gen.cpp"
                    break;

                    case 213:  // string: "textScore"
#line 1096 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3657 "parser_gen.cpp"
                    break;

                    case 214:  // aggregationFieldPath: "$-prefixed string"
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
#line 3673 "parser_gen.cpp"
                    break;

                    case 215:  // variable: "$$-prefixed string"
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
#line 3689 "parser_gen.cpp"
                    break;

                    case 216:  // binary: "BinData"
#line 1130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3697 "parser_gen.cpp"
                    break;

                    case 217:  // undefined: "undefined"
#line 1136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3705 "parser_gen.cpp"
                    break;

                    case 218:  // objectId: "ObjectID"
#line 1142 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3713 "parser_gen.cpp"
                    break;

                    case 219:  // date: "Date"
#line 1148 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3721 "parser_gen.cpp"
                    break;

                    case 220:  // null: "null"
#line 1154 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3729 "parser_gen.cpp"
                    break;

                    case 221:  // regex: "regex"
#line 1160 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3737 "parser_gen.cpp"
                    break;

                    case 222:  // dbPointer: "dbPointer"
#line 1166 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3745 "parser_gen.cpp"
                    break;

                    case 223:  // javascript: "Code"
#line 1172 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3753 "parser_gen.cpp"
                    break;

                    case 224:  // symbol: "Symbol"
#line 1178 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3761 "parser_gen.cpp"
                    break;

                    case 225:  // javascriptWScope: "CodeWScope"
#line 1184 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3769 "parser_gen.cpp"
                    break;

                    case 226:  // timestamp: "Timestamp"
#line 1190 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3777 "parser_gen.cpp"
                    break;

                    case 227:  // minKey: "minKey"
#line 1196 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3785 "parser_gen.cpp"
                    break;

                    case 228:  // maxKey: "maxKey"
#line 1202 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3793 "parser_gen.cpp"
                    break;

                    case 229:  // int: "arbitrary integer"
#line 1208 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3801 "parser_gen.cpp"
                    break;

                    case 230:  // int: "zero (int)"
#line 1211 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3809 "parser_gen.cpp"
                    break;

                    case 231:  // int: "1 (int)"
#line 1214 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3817 "parser_gen.cpp"
                    break;

                    case 232:  // int: "-1 (int)"
#line 1217 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3825 "parser_gen.cpp"
                    break;

                    case 233:  // long: "arbitrary long"
#line 1223 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3833 "parser_gen.cpp"
                    break;

                    case 234:  // long: "zero (long)"
#line 1226 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3841 "parser_gen.cpp"
                    break;

                    case 235:  // long: "1 (long)"
#line 1229 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3849 "parser_gen.cpp"
                    break;

                    case 236:  // long: "-1 (long)"
#line 1232 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3857 "parser_gen.cpp"
                    break;

                    case 237:  // double: "arbitrary double"
#line 1238 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3865 "parser_gen.cpp"
                    break;

                    case 238:  // double: "zero (double)"
#line 1241 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3873 "parser_gen.cpp"
                    break;

                    case 239:  // double: "1 (double)"
#line 1244 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3881 "parser_gen.cpp"
                    break;

                    case 240:  // double: "-1 (double)"
#line 1247 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3889 "parser_gen.cpp"
                    break;

                    case 241:  // decimal: "arbitrary decimal"
#line 1253 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3897 "parser_gen.cpp"
                    break;

                    case 242:  // decimal: "zero (decimal)"
#line 1256 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3905 "parser_gen.cpp"
                    break;

                    case 243:  // decimal: "1 (decimal)"
#line 1259 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3913 "parser_gen.cpp"
                    break;

                    case 244:  // decimal: "-1 (decimal)"
#line 1262 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3921 "parser_gen.cpp"
                    break;

                    case 245:  // bool: "true"
#line 1268 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3929 "parser_gen.cpp"
                    break;

                    case 246:  // bool: "false"
#line 1271 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3937 "parser_gen.cpp"
                    break;

                    case 247:  // simpleValue: string
#line 1277 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3943 "parser_gen.cpp"
                    break;

                    case 248:  // simpleValue: aggregationFieldPath
#line 1278 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3949 "parser_gen.cpp"
                    break;

                    case 249:  // simpleValue: variable
#line 1279 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3955 "parser_gen.cpp"
                    break;

                    case 250:  // simpleValue: binary
#line 1280 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3961 "parser_gen.cpp"
                    break;

                    case 251:  // simpleValue: undefined
#line 1281 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3967 "parser_gen.cpp"
                    break;

                    case 252:  // simpleValue: objectId
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3973 "parser_gen.cpp"
                    break;

                    case 253:  // simpleValue: date
#line 1283 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3979 "parser_gen.cpp"
                    break;

                    case 254:  // simpleValue: null
#line 1284 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3985 "parser_gen.cpp"
                    break;

                    case 255:  // simpleValue: regex
#line 1285 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3991 "parser_gen.cpp"
                    break;

                    case 256:  // simpleValue: dbPointer
#line 1286 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3997 "parser_gen.cpp"
                    break;

                    case 257:  // simpleValue: javascript
#line 1287 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4003 "parser_gen.cpp"
                    break;

                    case 258:  // simpleValue: symbol
#line 1288 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4009 "parser_gen.cpp"
                    break;

                    case 259:  // simpleValue: javascriptWScope
#line 1289 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4015 "parser_gen.cpp"
                    break;

                    case 260:  // simpleValue: int
#line 1290 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4021 "parser_gen.cpp"
                    break;

                    case 261:  // simpleValue: long
#line 1291 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4027 "parser_gen.cpp"
                    break;

                    case 262:  // simpleValue: double
#line 1292 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4033 "parser_gen.cpp"
                    break;

                    case 263:  // simpleValue: decimal
#line 1293 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4039 "parser_gen.cpp"
                    break;

                    case 264:  // simpleValue: bool
#line 1294 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4045 "parser_gen.cpp"
                    break;

                    case 265:  // simpleValue: timestamp
#line 1295 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4051 "parser_gen.cpp"
                    break;

                    case 266:  // simpleValue: minKey
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4057 "parser_gen.cpp"
                    break;

                    case 267:  // simpleValue: maxKey
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4063 "parser_gen.cpp"
                    break;

                    case 268:  // expressions: %empty
#line 1304 "grammar.yy"
                    {
                    }
#line 4069 "parser_gen.cpp"
                    break;

                    case 269:  // expressions: expressions expression
#line 1305 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4078 "parser_gen.cpp"
                    break;

                    case 270:  // expression: simpleValue
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4084 "parser_gen.cpp"
                    break;

                    case 271:  // expression: expressionObject
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4090 "parser_gen.cpp"
                    break;

                    case 272:  // expression: expressionArray
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4096 "parser_gen.cpp"
                    break;

                    case 273:  // expression: nonArrayNonObjCompoundExpression
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4102 "parser_gen.cpp"
                    break;

                    case 274:  // nonArrayExpression: simpleValue
#line 1316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4108 "parser_gen.cpp"
                    break;

                    case 275:  // nonArrayExpression: nonArrayCompoundExpression
#line 1316 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4114 "parser_gen.cpp"
                    break;

                    case 276:  // nonArrayCompoundExpression: expressionObject
#line 1320 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4120 "parser_gen.cpp"
                    break;

                    case 277:  // nonArrayCompoundExpression: nonArrayNonObjCompoundExpression
#line 1320 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4126 "parser_gen.cpp"
                    break;

                    case 278:  // nonArrayNonObjCompoundExpression: arrayManipulation
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4132 "parser_gen.cpp"
                    break;

                    case 279:  // nonArrayNonObjCompoundExpression: maths
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4138 "parser_gen.cpp"
                    break;

                    case 280:  // nonArrayNonObjCompoundExpression: meta
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4144 "parser_gen.cpp"
                    break;

                    case 281:  // nonArrayNonObjCompoundExpression: boolExprs
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4150 "parser_gen.cpp"
                    break;

                    case 282:  // nonArrayNonObjCompoundExpression: literalEscapes
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4156 "parser_gen.cpp"
                    break;

                    case 283:  // nonArrayNonObjCompoundExpression: compExprs
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4162 "parser_gen.cpp"
                    break;

                    case 284:  // nonArrayNonObjCompoundExpression: typeExpression
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4168 "parser_gen.cpp"
                    break;

                    case 285:  // nonArrayNonObjCompoundExpression: stringExps
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4174 "parser_gen.cpp"
                    break;

                    case 286:  // nonArrayNonObjCompoundExpression: setExpression
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4180 "parser_gen.cpp"
                    break;

                    case 287:  // nonArrayNonObjCompoundExpression: trig
#line 1325 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4186 "parser_gen.cpp"
                    break;

                    case 288:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1330 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4194 "parser_gen.cpp"
                    break;

                    case 289:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1337 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4202 "parser_gen.cpp"
                    break;

                    case 290:  // compoundNonObjectExpression: expressionArray
#line 1343 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4208 "parser_gen.cpp"
                    break;

                    case 291:  // compoundNonObjectExpression: nonArrayNonObjCompoundExpression
#line 1343 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4214 "parser_gen.cpp"
                    break;

                    case 292:  // arrayManipulation: slice
#line 1347 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4220 "parser_gen.cpp"
                    break;

                    case 293:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4229 "parser_gen.cpp"
                    break;

                    case 294:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1355 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4238 "parser_gen.cpp"
                    break;

                    case 295:  // expressionArray: "array" expressions "end of array"
#line 1364 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4246 "parser_gen.cpp"
                    break;

                    case 296:  // expressionSingletonArray: "array" expression "end of array"
#line 1371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4254 "parser_gen.cpp"
                    break;

                    case 297:  // singleArgExpression: nonArrayExpression
#line 1376 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4260 "parser_gen.cpp"
                    break;

                    case 298:  // singleArgExpression: expressionSingletonArray
#line 1376 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4266 "parser_gen.cpp"
                    break;

                    case 299:  // expressionObject: "object" expressionFields "end of object"
#line 1381 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4274 "parser_gen.cpp"
                    break;

                    case 300:  // expressionFields: %empty
#line 1387 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4282 "parser_gen.cpp"
                    break;

                    case 301:  // expressionFields: expressionFields expressionField
#line 1390 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4291 "parser_gen.cpp"
                    break;

                    case 302:  // expressionField: expressionFieldname expression
#line 1397 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4299 "parser_gen.cpp"
                    break;

                    case 303:  // expressionFieldname: invariableUserFieldname
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4305 "parser_gen.cpp"
                    break;

                    case 304:  // expressionFieldname: stageAsUserFieldname
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4311 "parser_gen.cpp"
                    break;

                    case 305:  // expressionFieldname: argAsUserFieldname
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4317 "parser_gen.cpp"
                    break;

                    case 306:  // expressionFieldname: idAsUserFieldname
#line 1404 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4323 "parser_gen.cpp"
                    break;

                    case 307:  // idAsUserFieldname: ID
#line 1408 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4331 "parser_gen.cpp"
                    break;

                    case 308:  // idAsProjectionPath: ID
#line 1414 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 4339 "parser_gen.cpp"
                    break;

                    case 309:  // maths: add
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4345 "parser_gen.cpp"
                    break;

                    case 310:  // maths: abs
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4351 "parser_gen.cpp"
                    break;

                    case 311:  // maths: ceil
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4357 "parser_gen.cpp"
                    break;

                    case 312:  // maths: divide
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4363 "parser_gen.cpp"
                    break;

                    case 313:  // maths: exponent
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4369 "parser_gen.cpp"
                    break;

                    case 314:  // maths: floor
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4375 "parser_gen.cpp"
                    break;

                    case 315:  // maths: ln
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4381 "parser_gen.cpp"
                    break;

                    case 316:  // maths: log
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4387 "parser_gen.cpp"
                    break;

                    case 317:  // maths: logten
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4393 "parser_gen.cpp"
                    break;

                    case 318:  // maths: mod
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4399 "parser_gen.cpp"
                    break;

                    case 319:  // maths: multiply
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4405 "parser_gen.cpp"
                    break;

                    case 320:  // maths: pow
#line 1420 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4411 "parser_gen.cpp"
                    break;

                    case 321:  // maths: round
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4417 "parser_gen.cpp"
                    break;

                    case 322:  // maths: sqrt
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4423 "parser_gen.cpp"
                    break;

                    case 323:  // maths: subtract
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4429 "parser_gen.cpp"
                    break;

                    case 324:  // maths: trunc
#line 1421 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4435 "parser_gen.cpp"
                    break;

                    case 325:  // meta: "object" META "geoNearDistance" "end of object"
#line 1425 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4443 "parser_gen.cpp"
                    break;

                    case 326:  // meta: "object" META "geoNearPoint" "end of object"
#line 1428 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4451 "parser_gen.cpp"
                    break;

                    case 327:  // meta: "object" META "indexKey" "end of object"
#line 1431 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4459 "parser_gen.cpp"
                    break;

                    case 328:  // meta: "object" META "randVal" "end of object"
#line 1434 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4467 "parser_gen.cpp"
                    break;

                    case 329:  // meta: "object" META "recordId" "end of object"
#line 1437 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4475 "parser_gen.cpp"
                    break;

                    case 330:  // meta: "object" META "searchHighlights" "end of object"
#line 1440 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4483 "parser_gen.cpp"
                    break;

                    case 331:  // meta: "object" META "searchScore" "end of object"
#line 1443 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4491 "parser_gen.cpp"
                    break;

                    case 332:  // meta: "object" META "sortKey" "end of object"
#line 1446 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4499 "parser_gen.cpp"
                    break;

                    case 333:  // meta: "object" META "textScore" "end of object"
#line 1449 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4507 "parser_gen.cpp"
                    break;

                    case 334:  // trig: sin
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4513 "parser_gen.cpp"
                    break;

                    case 335:  // trig: cos
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4519 "parser_gen.cpp"
                    break;

                    case 336:  // trig: tan
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4525 "parser_gen.cpp"
                    break;

                    case 337:  // trig: sinh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4531 "parser_gen.cpp"
                    break;

                    case 338:  // trig: cosh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4537 "parser_gen.cpp"
                    break;

                    case 339:  // trig: tanh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4543 "parser_gen.cpp"
                    break;

                    case 340:  // trig: asin
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4549 "parser_gen.cpp"
                    break;

                    case 341:  // trig: acos
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4555 "parser_gen.cpp"
                    break;

                    case 342:  // trig: atan
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4561 "parser_gen.cpp"
                    break;

                    case 343:  // trig: atan2
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4567 "parser_gen.cpp"
                    break;

                    case 344:  // trig: asinh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4573 "parser_gen.cpp"
                    break;

                    case 345:  // trig: acosh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4579 "parser_gen.cpp"
                    break;

                    case 346:  // trig: atanh
#line 1454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4585 "parser_gen.cpp"
                    break;

                    case 347:  // trig: degreesToRadians
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4591 "parser_gen.cpp"
                    break;

                    case 348:  // trig: radiansToDegrees
#line 1455 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4597 "parser_gen.cpp"
                    break;

                    case 349:  // add: "object" ADD expressionArray "end of object"
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4606 "parser_gen.cpp"
                    break;

                    case 350:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1466 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4615 "parser_gen.cpp"
                    break;

                    case 351:  // abs: "object" ABS expression "end of object"
#line 1472 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4623 "parser_gen.cpp"
                    break;

                    case 352:  // ceil: "object" CEIL expression "end of object"
#line 1477 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4631 "parser_gen.cpp"
                    break;

                    case 353:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4640 "parser_gen.cpp"
                    break;

                    case 354:  // exponent: "object" EXPONENT expression "end of object"
#line 1488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4648 "parser_gen.cpp"
                    break;

                    case 355:  // floor: "object" FLOOR expression "end of object"
#line 1493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4656 "parser_gen.cpp"
                    break;

                    case 356:  // ln: "object" LN expression "end of object"
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4664 "parser_gen.cpp"
                    break;

                    case 357:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1503 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4673 "parser_gen.cpp"
                    break;

                    case 358:  // logten: "object" LOGTEN expression "end of object"
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4681 "parser_gen.cpp"
                    break;

                    case 359:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1514 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4690 "parser_gen.cpp"
                    break;

                    case 360:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
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
#line 4702 "parser_gen.cpp"
                    break;

                    case 361:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4711 "parser_gen.cpp"
                    break;

                    case 362:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1535 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4720 "parser_gen.cpp"
                    break;

                    case 363:  // sqrt: "object" SQRT expression "end of object"
#line 1541 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4728 "parser_gen.cpp"
                    break;

                    case 364:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4737 "parser_gen.cpp"
                    break;

                    case 365:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1552 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4746 "parser_gen.cpp"
                    break;

                    case 366:  // sin: "object" SIN singleArgExpression "end of object"
#line 1558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4754 "parser_gen.cpp"
                    break;

                    case 367:  // cos: "object" COS singleArgExpression "end of object"
#line 1563 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4762 "parser_gen.cpp"
                    break;

                    case 368:  // tan: "object" TAN singleArgExpression "end of object"
#line 1568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4770 "parser_gen.cpp"
                    break;

                    case 369:  // sinh: "object" SINH singleArgExpression "end of object"
#line 1573 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4778 "parser_gen.cpp"
                    break;

                    case 370:  // cosh: "object" COSH singleArgExpression "end of object"
#line 1578 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4786 "parser_gen.cpp"
                    break;

                    case 371:  // tanh: "object" TANH singleArgExpression "end of object"
#line 1583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4794 "parser_gen.cpp"
                    break;

                    case 372:  // asin: "object" ASIN singleArgExpression "end of object"
#line 1588 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4802 "parser_gen.cpp"
                    break;

                    case 373:  // acos: "object" ACOS singleArgExpression "end of object"
#line 1593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4810 "parser_gen.cpp"
                    break;

                    case 374:  // atan: "object" ATAN singleArgExpression "end of object"
#line 1598 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4818 "parser_gen.cpp"
                    break;

                    case 375:  // asinh: "object" ASINH singleArgExpression "end of object"
#line 1603 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4826 "parser_gen.cpp"
                    break;

                    case 376:  // acosh: "object" ACOSH singleArgExpression "end of object"
#line 1608 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4834 "parser_gen.cpp"
                    break;

                    case 377:  // atanh: "object" ATANH singleArgExpression "end of object"
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4842 "parser_gen.cpp"
                    break;

                    case 378:  // degreesToRadians: "object" DEGREES_TO_RADIANS singleArgExpression
                               // "end of object"
#line 1618 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4850 "parser_gen.cpp"
                    break;

                    case 379:  // radiansToDegrees: "object" RADIANS_TO_DEGREES singleArgExpression
                               // "end of object"
#line 1623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4858 "parser_gen.cpp"
                    break;

                    case 380:  // boolExprs: and
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4864 "parser_gen.cpp"
                    break;

                    case 381:  // boolExprs: or
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4870 "parser_gen.cpp"
                    break;

                    case 382:  // boolExprs: not
#line 1629 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4876 "parser_gen.cpp"
                    break;

                    case 383:  // and: "object" AND expressionArray "end of object"
#line 1633 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4885 "parser_gen.cpp"
                    break;

                    case 384:  // or: "object" OR expressionArray "end of object"
#line 1640 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4894 "parser_gen.cpp"
                    break;

                    case 385:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4903 "parser_gen.cpp"
                    break;

                    case 386:  // stringExps: concat
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4909 "parser_gen.cpp"
                    break;

                    case 387:  // stringExps: dateFromString
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4915 "parser_gen.cpp"
                    break;

                    case 388:  // stringExps: dateToString
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4921 "parser_gen.cpp"
                    break;

                    case 389:  // stringExps: indexOfBytes
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4927 "parser_gen.cpp"
                    break;

                    case 390:  // stringExps: indexOfCP
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4933 "parser_gen.cpp"
                    break;

                    case 391:  // stringExps: ltrim
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4939 "parser_gen.cpp"
                    break;

                    case 392:  // stringExps: regexFind
#line 1654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4945 "parser_gen.cpp"
                    break;

                    case 393:  // stringExps: regexFindAll
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4951 "parser_gen.cpp"
                    break;

                    case 394:  // stringExps: regexMatch
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4957 "parser_gen.cpp"
                    break;

                    case 395:  // stringExps: replaceOne
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4963 "parser_gen.cpp"
                    break;

                    case 396:  // stringExps: replaceAll
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4969 "parser_gen.cpp"
                    break;

                    case 397:  // stringExps: rtrim
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4975 "parser_gen.cpp"
                    break;

                    case 398:  // stringExps: split
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4981 "parser_gen.cpp"
                    break;

                    case 399:  // stringExps: strLenBytes
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4987 "parser_gen.cpp"
                    break;

                    case 400:  // stringExps: strLenCP
#line 1655 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4993 "parser_gen.cpp"
                    break;

                    case 401:  // stringExps: strcasecmp
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4999 "parser_gen.cpp"
                    break;

                    case 402:  // stringExps: substr
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5005 "parser_gen.cpp"
                    break;

                    case 403:  // stringExps: substrBytes
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5011 "parser_gen.cpp"
                    break;

                    case 404:  // stringExps: substrCP
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5017 "parser_gen.cpp"
                    break;

                    case 405:  // stringExps: toLower
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5023 "parser_gen.cpp"
                    break;

                    case 406:  // stringExps: trim
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5029 "parser_gen.cpp"
                    break;

                    case 407:  // stringExps: toUpper
#line 1656 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5035 "parser_gen.cpp"
                    break;

                    case 408:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1660 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5047 "parser_gen.cpp"
                    break;

                    case 409:  // formatArg: %empty
#line 1670 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5055 "parser_gen.cpp"
                    break;

                    case 410:  // formatArg: "format argument" expression
#line 1673 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5063 "parser_gen.cpp"
                    break;

                    case 411:  // timezoneArg: %empty
#line 1679 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5071 "parser_gen.cpp"
                    break;

                    case 412:  // timezoneArg: "timezone argument" expression
#line 1682 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5079 "parser_gen.cpp"
                    break;

                    case 413:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
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
#line 5089 "parser_gen.cpp"
                    break;

                    case 414:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
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
#line 5099 "parser_gen.cpp"
                    break;

                    case 415:  // exprZeroToTwo: %empty
#line 1706 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 5107 "parser_gen.cpp"
                    break;

                    case 416:  // exprZeroToTwo: expression
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5115 "parser_gen.cpp"
                    break;

                    case 417:  // exprZeroToTwo: expression expression
#line 1712 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5123 "parser_gen.cpp"
                    break;

                    case 418:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
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
#line 5135 "parser_gen.cpp"
                    break;

                    case 419:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
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
#line 5147 "parser_gen.cpp"
                    break;

                    case 420:  // charsArg: %empty
#line 1740 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 5155 "parser_gen.cpp"
                    break;

                    case 421:  // charsArg: "chars argument" expression
#line 1743 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5163 "parser_gen.cpp"
                    break;

                    case 422:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1749 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5173 "parser_gen.cpp"
                    break;

                    case 423:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1757 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5183 "parser_gen.cpp"
                    break;

                    case 424:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 1765 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5193 "parser_gen.cpp"
                    break;

                    case 425:  // optionsArg: %empty
#line 1773 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 5201 "parser_gen.cpp"
                    break;

                    case 426:  // optionsArg: "options argument" expression
#line 1776 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5209 "parser_gen.cpp"
                    break;

                    case 427:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 1781 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 5221 "parser_gen.cpp"
                    break;

                    case 428:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 1790 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5229 "parser_gen.cpp"
                    break;

                    case 429:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 1796 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5237 "parser_gen.cpp"
                    break;

                    case 430:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 1802 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5245 "parser_gen.cpp"
                    break;

                    case 431:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
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
#line 5256 "parser_gen.cpp"
                    break;

                    case 432:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
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
#line 5267 "parser_gen.cpp"
                    break;

                    case 433:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 1828 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5276 "parser_gen.cpp"
                    break;

                    case 434:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 1835 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5285 "parser_gen.cpp"
                    break;

                    case 435:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 1842 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5294 "parser_gen.cpp"
                    break;

                    case 436:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 1850 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5303 "parser_gen.cpp"
                    break;

                    case 437:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 1858 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5312 "parser_gen.cpp"
                    break;

                    case 438:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 1866 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5321 "parser_gen.cpp"
                    break;

                    case 439:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5330 "parser_gen.cpp"
                    break;

                    case 440:  // toLower: "object" TO_LOWER expression "end of object"
#line 1881 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5338 "parser_gen.cpp"
                    break;

                    case 441:  // toUpper: "object" TO_UPPER expression "end of object"
#line 1887 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5346 "parser_gen.cpp"
                    break;

                    case 442:  // metaSortKeyword: "randVal"
#line 1893 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 5354 "parser_gen.cpp"
                    break;

                    case 443:  // metaSortKeyword: "textScore"
#line 1896 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 5362 "parser_gen.cpp"
                    break;

                    case 444:  // metaSort: "object" META metaSortKeyword "end of object"
#line 1902 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5370 "parser_gen.cpp"
                    break;

                    case 445:  // sortSpecs: "object" specList "end of object"
#line 1908 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5378 "parser_gen.cpp"
                    break;

                    case 446:  // specList: %empty
#line 1913 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5386 "parser_gen.cpp"
                    break;

                    case 447:  // specList: specList sortSpec
#line 1916 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5395 "parser_gen.cpp"
                    break;

                    case 448:  // oneOrNegOne: "1 (int)"
#line 1923 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 5403 "parser_gen.cpp"
                    break;

                    case 449:  // oneOrNegOne: "-1 (int)"
#line 1926 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 5411 "parser_gen.cpp"
                    break;

                    case 450:  // oneOrNegOne: "1 (long)"
#line 1929 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 5419 "parser_gen.cpp"
                    break;

                    case 451:  // oneOrNegOne: "-1 (long)"
#line 1932 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 5427 "parser_gen.cpp"
                    break;

                    case 452:  // oneOrNegOne: "1 (double)"
#line 1935 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 5435 "parser_gen.cpp"
                    break;

                    case 453:  // oneOrNegOne: "-1 (double)"
#line 1938 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 5443 "parser_gen.cpp"
                    break;

                    case 454:  // oneOrNegOne: "1 (decimal)"
#line 1941 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 5451 "parser_gen.cpp"
                    break;

                    case 455:  // oneOrNegOne: "-1 (decimal)"
#line 1944 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 5459 "parser_gen.cpp"
                    break;

                    case 456:  // sortFieldname: valueFieldname
#line 1949 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 5467 "parser_gen.cpp"
                    break;

                    case 457:  // sortFieldname: "fieldname containing dotted path"
#line 1951 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 5479 "parser_gen.cpp"
                    break;

                    case 458:  // sortSpec: sortFieldname metaSort
#line 1961 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5487 "parser_gen.cpp"
                    break;

                    case 459:  // sortSpec: sortFieldname oneOrNegOne
#line 1963 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5495 "parser_gen.cpp"
                    break;

                    case 460:  // setExpression: allElementsTrue
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5501 "parser_gen.cpp"
                    break;

                    case 461:  // setExpression: anyElementTrue
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5507 "parser_gen.cpp"
                    break;

                    case 462:  // setExpression: setDifference
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5513 "parser_gen.cpp"
                    break;

                    case 463:  // setExpression: setEquals
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5519 "parser_gen.cpp"
                    break;

                    case 464:  // setExpression: setIntersection
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5525 "parser_gen.cpp"
                    break;

                    case 465:  // setExpression: setIsSubset
#line 1969 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5531 "parser_gen.cpp"
                    break;

                    case 466:  // setExpression: setUnion
#line 1970 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5537 "parser_gen.cpp"
                    break;

                    case 467:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 1974 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5545 "parser_gen.cpp"
                    break;

                    case 468:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 1980 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5553 "parser_gen.cpp"
                    break;

                    case 469:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 1986 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5562 "parser_gen.cpp"
                    break;

                    case 470:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 1994 "grammar.yy"
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
#line 5574 "parser_gen.cpp"
                    break;

                    case 471:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 2005 "grammar.yy"
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
#line 5586 "parser_gen.cpp"
                    break;

                    case 472:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 2015 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5595 "parser_gen.cpp"
                    break;

                    case 473:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2023 "grammar.yy"
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
#line 5607 "parser_gen.cpp"
                    break;

                    case 474:  // literalEscapes: const
#line 2033 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5613 "parser_gen.cpp"
                    break;

                    case 475:  // literalEscapes: literal
#line 2033 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5619 "parser_gen.cpp"
                    break;

                    case 476:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 2037 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5628 "parser_gen.cpp"
                    break;

                    case 477:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 2044 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5637 "parser_gen.cpp"
                    break;

                    case 478:  // value: simpleValue
#line 2051 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5643 "parser_gen.cpp"
                    break;

                    case 479:  // value: compoundValue
#line 2051 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5649 "parser_gen.cpp"
                    break;

                    case 480:  // compoundValue: valueArray
#line 2055 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5655 "parser_gen.cpp"
                    break;

                    case 481:  // compoundValue: valueObject
#line 2055 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5661 "parser_gen.cpp"
                    break;

                    case 482:  // valueArray: "array" values "end of array"
#line 2059 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5669 "parser_gen.cpp"
                    break;

                    case 483:  // values: %empty
#line 2065 "grammar.yy"
                    {
                    }
#line 5675 "parser_gen.cpp"
                    break;

                    case 484:  // values: values value
#line 2066 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 5684 "parser_gen.cpp"
                    break;

                    case 485:  // valueObject: "object" valueFields "end of object"
#line 2073 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5692 "parser_gen.cpp"
                    break;

                    case 486:  // valueFields: %empty
#line 2079 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5700 "parser_gen.cpp"
                    break;

                    case 487:  // valueFields: valueFields valueField
#line 2082 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5709 "parser_gen.cpp"
                    break;

                    case 488:  // valueField: valueFieldname value
#line 2089 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5717 "parser_gen.cpp"
                    break;

                    case 489:  // valueFieldname: invariableUserFieldname
#line 2096 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5723 "parser_gen.cpp"
                    break;

                    case 490:  // valueFieldname: stageAsUserFieldname
#line 2097 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5729 "parser_gen.cpp"
                    break;

                    case 491:  // valueFieldname: argAsUserFieldname
#line 2098 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5735 "parser_gen.cpp"
                    break;

                    case 492:  // valueFieldname: aggExprAsUserFieldname
#line 2099 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5741 "parser_gen.cpp"
                    break;

                    case 493:  // valueFieldname: idAsUserFieldname
#line 2100 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5747 "parser_gen.cpp"
                    break;

                    case 494:  // compExprs: cmp
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5753 "parser_gen.cpp"
                    break;

                    case 495:  // compExprs: eq
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5759 "parser_gen.cpp"
                    break;

                    case 496:  // compExprs: gt
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5765 "parser_gen.cpp"
                    break;

                    case 497:  // compExprs: gte
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5771 "parser_gen.cpp"
                    break;

                    case 498:  // compExprs: lt
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5777 "parser_gen.cpp"
                    break;

                    case 499:  // compExprs: lte
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5783 "parser_gen.cpp"
                    break;

                    case 500:  // compExprs: ne
#line 2103 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5789 "parser_gen.cpp"
                    break;

                    case 501:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 2105 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5798 "parser_gen.cpp"
                    break;

                    case 502:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 2110 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5807 "parser_gen.cpp"
                    break;

                    case 503:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 2115 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5816 "parser_gen.cpp"
                    break;

                    case 504:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 2120 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5825 "parser_gen.cpp"
                    break;

                    case 505:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 2125 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5834 "parser_gen.cpp"
                    break;

                    case 506:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 2130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5843 "parser_gen.cpp"
                    break;

                    case 507:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 2135 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5852 "parser_gen.cpp"
                    break;

                    case 508:  // typeExpression: convert
#line 2141 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5858 "parser_gen.cpp"
                    break;

                    case 509:  // typeExpression: toBool
#line 2142 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5864 "parser_gen.cpp"
                    break;

                    case 510:  // typeExpression: toDate
#line 2143 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5870 "parser_gen.cpp"
                    break;

                    case 511:  // typeExpression: toDecimal
#line 2144 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5876 "parser_gen.cpp"
                    break;

                    case 512:  // typeExpression: toDouble
#line 2145 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5882 "parser_gen.cpp"
                    break;

                    case 513:  // typeExpression: toInt
#line 2146 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5888 "parser_gen.cpp"
                    break;

                    case 514:  // typeExpression: toLong
#line 2147 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5894 "parser_gen.cpp"
                    break;

                    case 515:  // typeExpression: toObjectId
#line 2148 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5900 "parser_gen.cpp"
                    break;

                    case 516:  // typeExpression: toString
#line 2149 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5906 "parser_gen.cpp"
                    break;

                    case 517:  // typeExpression: type
#line 2150 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5912 "parser_gen.cpp"
                    break;

                    case 518:  // onErrorArg: %empty
#line 2155 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 5920 "parser_gen.cpp"
                    break;

                    case 519:  // onErrorArg: "onError argument" expression
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5928 "parser_gen.cpp"
                    break;

                    case 520:  // onNullArg: %empty
#line 2165 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 5936 "parser_gen.cpp"
                    break;

                    case 521:  // onNullArg: "onNull argument" expression
#line 2168 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5944 "parser_gen.cpp"
                    break;

                    case 522:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2175 "grammar.yy"
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
#line 5955 "parser_gen.cpp"
                    break;

                    case 523:  // toBool: "object" TO_BOOL expression "end of object"
#line 2184 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5963 "parser_gen.cpp"
                    break;

                    case 524:  // toDate: "object" TO_DATE expression "end of object"
#line 2189 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5971 "parser_gen.cpp"
                    break;

                    case 525:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2194 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5979 "parser_gen.cpp"
                    break;

                    case 526:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 2199 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5987 "parser_gen.cpp"
                    break;

                    case 527:  // toInt: "object" TO_INT expression "end of object"
#line 2204 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5995 "parser_gen.cpp"
                    break;

                    case 528:  // toLong: "object" TO_LONG expression "end of object"
#line 2209 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6003 "parser_gen.cpp"
                    break;

                    case 529:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 2214 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6011 "parser_gen.cpp"
                    break;

                    case 530:  // toString: "object" TO_STRING expression "end of object"
#line 2219 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6019 "parser_gen.cpp"
                    break;

                    case 531:  // type: "object" TYPE expression "end of object"
#line 2224 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6027 "parser_gen.cpp"
                    break;


#line 6031 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -794;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    -50,  -76,  -71,  -69,  52,   -60,  -794, -794, -794, -794, -794, -794, 23,   13,   1133, 566,
    -49,  57,   -48,  -40,  57,   -794, 26,   -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, 2561, -794, -794, -794, -34,  -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, 161,  -794, -794, -794,
    40,   -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, 79,   -794, 92,   0,    -60,  -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, 32,   -794, -794, -794, 2806, 57,   54,   -794,
    -794, -5,   -71,  -75,  -794, 2691, -794, -794, 2691, -794, -794, -794, -794, 64,   98,   -794,
    -87,  -794, -794, 74,   -794, -794, 75,   -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, 730,  -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -18,  -794,
    -794, -794, -794, 1131, 2171, 2301, 2301, 12,   24,   12,   28,   2301, 2301, 2301, 31,   2301,
    2171, 31,   34,   37,   -794, 2301, 2301, -794, -794, 2301, 38,   31,   2171, 2171, 31,   31,
    -794, 39,   41,   42,   2171, 44,   2171, 31,   31,   -794, 313,  45,   49,   31,   51,   12,
    53,   2301, -794, -794, -794, -794, -794, 55,   -794, 31,   56,   58,   31,   59,   61,   2301,
    2301, 62,   2171, 69,   2171, 2171, 71,   73,   76,   81,   2301, 2301, 2171, 2171, 2171, 2171,
    2171, 2171, 2171, 2171, 2171, 2171, -794, 84,   2171, 2691, 2691, -794, 2856, 94,   72,   -794,
    1003, -794, -794, -794, -794, -794, 108,  2171, -794, -794, -794, -794, -794, -794, 116,  119,
    137,  2171, 147,  2171, 148,  149,  150,  2171, 151,  154,  155,  159,  -794, 2431, 199,  165,
    166,  205,  207,  169,  2171, 171,  172,  175,  177,  178,  2171, 2171, 2431, 179,  2171, 182,
    183,  184,  226,  186,  188,  190,  191,  197,  198,  203,  204,  208,  2171, 2171, 211,  2171,
    222,  2171, 223,  227,  261,  238,  246,  294,  295,  2171, 226,  260,  2171, 2171, 263,  2171,
    2171, 265,  266,  270,  275,  2171, 276,  2171, 277,  278,  2171, 2171, 2171, 2171, 279,  281,
    282,  285,  286,  291,  292,  293,  296,  297,  298,  299,  226,  2171, 300,  -794, -794, -794,
    -794, -794, 301,  2872, -794, 303,  -794, -794, -794, 305,  -794, 306,  -794, -794, -794, 2171,
    -794, -794, -794, -794, 1261, -794, -794, 307,  -794, -794, -794, -794, 2171, -794, -794, 2171,
    2171, -794, 2171, -794, -794, -794, -794, -794, 2171, 2171, 309,  -794, 2171, -794, -794, -794,
    2171, 330,  -794, -794, -794, -794, -794, -794, -794, -794, -794, 2171, 2171, -794, 310,  -794,
    2171, -794, -794, 2171, -794, -794, 2171, 2171, 2171, 348,  -794, 2171, 2171, -794, 2171, 2171,
    -794, -794, -794, -794, 2171, -794, 2171, -794, -794, 2171, 2171, 2171, 2171, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, 354,  2171, -794, -794, -794, 2171, -794,
    -794, -794, -794, -794, -794, 320,  326,  329,  334,  1391, 867,  336,  364,  373,  373,  339,
    2171, 2171, 340,  342,  -794, 2171, 343,  -794, 344,  350,  371,  385,  386,  353,  2171, -794,
    -794, -794, 1521, 355,  359,  2171, 2171, 2171, 361,  2171, 362,  -794, -794, -794, -794, -794,
    -794, -794, -794, 2431, -794, -794, 2171, 395,  2171, 380,  380,  365,  2171, 368,  370,  -794,
    375,  376,  377,  1651, -794, 378,  2171, 400,  2171, 2171, 381,  382,  1781, 1911, 2041, 384,
    387,  388,  391,  392,  393,  394,  396,  398,  -794, -794, 2171, 406,  -794, 2171, 364,  395,
    -794, -794, 399,  403,  -794, 405,  -794, 408,  -794, -794, 2171, 422,  424,  -794, 409,  410,
    411,  412,  -794, -794, -794, 413,  414,  415,  -794, 418,  -794, -794, 2171, -794, 395,  419,
    -794, -794, -794, -794, 420,  2171, 2171, -794, -794, -794, -794, -794, -794, -794, -794, 421,
    425,  427,  -794, 428,  429,  430,  431,  -794, 439,  440,  -794, -794, -794, -794};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   75,  3,   446, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   10,  11,  12,  13,  14,  15,  5,   87,  115, 104, 114, 111, 118, 112, 107,
    109, 110, 117, 105, 116, 119, 106, 113, 108, 74,  307, 89,  88,  95,  93,  94,  92,  0,   102,
    76,  78,  0,   144, 197, 200, 120, 183, 122, 184, 196, 199, 198, 121, 201, 145, 127, 160, 123,
    134, 191, 194, 161, 162, 202, 146, 445, 128, 147, 148, 129, 130, 163, 164, 124, 149, 150, 151,
    131, 132, 165, 166, 152, 153, 133, 126, 125, 154, 203, 167, 168, 169, 171, 170, 155, 172, 185,
    186, 187, 188, 189, 156, 190, 193, 173, 157, 96,  99,  100, 101, 98,  97,  176, 174, 175, 177,
    178, 179, 158, 192, 195, 135, 136, 137, 138, 139, 140, 180, 141, 142, 182, 181, 159, 143, 457,
    490, 491, 492, 489, 0,   493, 456, 447, 0,   244, 243, 242, 240, 239, 238, 232, 231, 230, 236,
    235, 234, 229, 233, 237, 241, 19,  20,  21,  22,  24,  26,  0,   23,  0,   0,   6,   246, 245,
    205, 206, 207, 208, 209, 210, 211, 212, 81,  213, 204, 214, 215, 216, 217, 218, 219, 220, 221,
    222, 223, 224, 225, 226, 227, 228, 256, 257, 258, 259, 260, 265, 261, 262, 263, 266, 267, 247,
    248, 250, 251, 252, 264, 253, 254, 255, 79,  249, 77,  90,  455, 454, 453, 452, 449, 448, 451,
    450, 0,   458, 459, 17,  0,   0,   0,   9,   7,   0,   0,   0,   25,  0,   66,  68,  0,   65,
    67,  27,  103, 0,   0,   80,  0,   82,  83,  91,  442, 443, 0,   59,  58,  55,  54,  57,  51,
    50,  53,  43,  42,  45,  47,  46,  49,  268, 0,   44,  48,  52,  56,  38,  39,  40,  41,  60,
    61,  62,  31,  32,  33,  34,  35,  36,  37,  28,  30,  63,  64,  278, 292, 290, 279, 280, 309,
    281, 380, 381, 382, 282, 474, 475, 285, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396,
    397, 398, 399, 400, 401, 402, 403, 404, 405, 407, 406, 283, 494, 495, 496, 497, 498, 499, 500,
    284, 508, 509, 510, 511, 512, 513, 514, 515, 516, 517, 310, 311, 312, 313, 314, 315, 316, 317,
    318, 319, 320, 321, 322, 323, 324, 286, 460, 461, 462, 463, 464, 465, 466, 287, 334, 335, 336,
    337, 338, 339, 340, 341, 342, 344, 345, 346, 343, 347, 348, 291, 29,  16,  0,   81,  84,  86,
    444, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8,
    0,   0,   8,   8,   0,   0,   0,   0,   0,   0,   0,   308, 0,   0,   0,   0,   0,   0,   0,
    0,   8,   0,   0,   0,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   0,   8,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   8,   0,   0,   0,   0,   70,  0,   0,   0,   295, 300,
    270, 269, 272, 271, 273, 0,   0,   274, 276, 297, 275, 277, 298, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   268, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   420, 0,   0,   73,  72,  69,  71,  18,  82,
    0,   351, 0,   373, 376, 349, 0,   383, 0,   372, 375, 374, 0,   350, 377, 352, 501, 0,   483,
    486, 0,   478, 479, 480, 481, 0,   367, 370, 0,   0,   378, 0,   502, 354, 355, 503, 504, 0,
    0,   0,   356, 0,   358, 505, 506, 0,   0,   325, 326, 327, 328, 329, 330, 331, 332, 333, 0,
    0,   507, 0,   384, 0,   379, 428, 0,   429, 430, 0,   0,   0,   0,   469, 0,   0,   472, 0,
    0,   293, 294, 366, 369, 0,   363, 0,   434, 435, 0,   0,   0,   0,   368, 371, 523, 524, 525,
    526, 527, 528, 440, 529, 530, 441, 0,   0,   531, 85,  299, 0,   304, 305, 303, 306, 301, 296,
    0,   0,   0,   0,   0,   0,   0,   518, 409, 409, 0,   415, 415, 0,   0,   421, 0,   0,   268,
    0,   0,   425, 0,   0,   0,   0,   268, 268, 268, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    302, 467, 468, 288, 408, 482, 484, 485, 0,   487, 476, 0,   520, 0,   411, 411, 0,   416, 0,
    0,   477, 0,   0,   0,   0,   385, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   488, 519, 0,   0,   410, 0,   518, 520, 353, 417, 0,   0,
    357, 0,   359, 0,   361, 426, 0,   0,   0,   362, 0,   0,   0,   0,   289, 433, 436, 0,   0,
    0,   364, 0,   365, 521, 0,   412, 520, 0,   418, 419, 422, 360, 0,   0,   0,   423, 470, 471,
    473, 437, 438, 439, 424, 0,   0,   0,   427, 0,   0,   0,   0,   414, 0,   0,   522, 413, 432,
    431};

const short ParserGen::yypgoto_[] = {
    -794, 240,  -794, -794, -176, -13,  -794, -794, -12,  -794, -11,  -794, -253, -794, -794,
    -1,   -794, -794, -247, -240, -236, -225, -220, -6,   -213, -2,   -8,   78,   -211, -206,
    -549, -248, -794, -200, -193, -186, -794, -171, -167, -238, -55,  -794, -794, -794, -794,
    -794, -794, 315,  -794, -794, -794, -794, -794, -794, -794, -794, -794, 233,  -449, -794,
    -3,   -412, -794, 66,   -794, -794, -794, -242, 67,   -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -408, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -330, -793, -244, -291, -551, -794, -444, -794, -243, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794, -794,
    -794, -794, -794, -794, -794, -794, -794, -794, 46,   -794, 907,  256,  -794, 100,  -794,
    -794, -794, -794, 6,    -794, -794, -794, -794, -794, -794, -794, -794, -794, -17,  -794};

const short ParserGen::yydefgoto_[] = {
    -1,  505, 265, 734, 152, 153, 266, 154, 155, 156, 157, 506, 158, 55,  267, 507, 739, 788,
    56,  216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 647, 227, 228, 229, 230, 231,
    232, 233, 234, 235, 513, 649, 650, 651, 746, 237, 6,   13,  22,  23,  24,  25,  26,  27,
    28,  252, 508, 313, 314, 315, 181, 514, 316, 536, 594, 317, 318, 515, 516, 627, 320, 321,
    322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339,
    579, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356,
    357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374,
    375, 376, 377, 378, 379, 380, 381, 382, 383, 384, 385, 386, 791, 827, 793, 830, 673, 807,
    419, 745, 797, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399, 400, 401,
    402, 403, 404, 405, 406, 407, 408, 409, 410, 522, 523, 517, 525, 526, 8,   14,  257, 238,
    258, 57,  58,  273, 274, 59,  10,  15,  249, 250, 278, 159, 4,   580, 186};

const short ParserGen::yytable_[] = {
    236, 52,  53,  54,  185, 268, 270, 666, 518, 179, 276, 177, 179, 306, 177, 178, 306, 184, 178,
    319, 538, 299, 319, 312, 299, 300, 312, 415, 300, 164, 165, 166, 550, 551, 416, 697, 301, 5,
    863, 301, 557, 302, 559, 7,   302, 9,   271, 268, 303, 277, 304, 303, 11,  304, 12,  305, 621,
    622, 305, 581, 582, 307, 208, 29,  307, 160, 182, 729, 308, 880, 598, 308, 600, 601, 183, 309,
    272, 187, 309, 239, 608, 609, 610, 611, 612, 613, 614, 615, 616, 617, 310, 251, 620, 310, 311,
    180, 644, 311, 180, 161, 162, 163, 253, 254, 164, 165, 166, 629, 259, 190, 191, 1,   2,   3,
    255, 413, 192, 633, 414, 635, 167, 168, 169, 639, 417, 293, 418, 170, 171, 172, 16,  17,  18,
    19,  20,  21,  658, 529, 175, 193, 194, 531, 664, 665, 535, 625, 668, 540, 195, 196, 541, 548,
    554, 272, 555, 556, 197, 558, 572, 628, 683, 684, 573, 686, 575, 688, 577, 630, 585, 588, 631,
    589, 591, 696, 592, 597, 699, 700, 199, 702, 703, 528, 599, 530, 602, 708, 603, 710, 632, 604,
    713, 714, 715, 716, 605, 200, 785, 619, 634, 636, 637, 638, 640, 240, 241, 641, 642, 730, 242,
    243, 643, 173, 174, 175, 176, 652, 653, 654, 655, 656, 657, 576, 659, 660, 244, 245, 661, 743,
    662, 663, 667, 246, 247, 669, 670, 671, 672, 674, 824, 675, 748, 676, 677, 749, 750, 179, 751,
    177, 678, 679, 269, 178, 752, 753, 680, 681, 755, 306, 306, 682, 756, 268, 685, 319, 319, 299,
    299, 312, 312, 300, 300, 758, 759, 687, 689, 248, 761, 691, 690, 762, 301, 301, 763, 764, 765,
    302, 302, 767, 768, 692, 769, 770, 303, 303, 304, 304, 771, 693, 772, 305, 305, 773, 774, 775,
    776, 307, 307, 411, 694, 695, 411, 698, 308, 308, 701, 803, 704, 705, 778, 309, 309, 706, 779,
    812, 813, 814, 707, 709, 711, 712, 717, 180, 718, 719, 310, 310, 720, 721, 311, 311, 796, 796,
    722, 723, 724, 801, 757, 725, 726, 727, 728, 731, 732, 740, 811, 741, 742, 747, 815, 754, 760,
    818, 819, 820, 766, 822, 520, 520, 563, 564, 777, 780, 520, 520, 520, 565, 520, 781, 825, 782,
    828, 790, 520, 520, 833, 783, 520, 789, 792, 795, 806, 799, 800, 802, 841, 804, 843, 844, 566,
    567, 805, 808, 809, 810, 829, 816, 509, 568, 569, 817, 520, 821, 823, 826, 859, 570, 832, 861,
    834, 542, 835, 842, 545, 546, 520, 520, 836, 837, 838, 840, 868, 860, 845, 846, 850, 520, 520,
    571, 851, 852, 562, 853, 854, 855, 869, 856, 870, 857, 879, 858, 864, 735, 583, 584, 865, 586,
    866, 883, 884, 867, 871, 872, 873, 874, 875, 876, 877, 524, 524, 878, 881, 882, 885, 524, 524,
    524, 886, 524, 887, 888, 889, 890, 891, 524, 524, 618, 648, 524, 521, 521, 892, 893, 264, 787,
    521, 521, 521, 412, 521, 539, 862, 648, 256, 831, 521, 521, 794, 624, 521, 549, 798, 524, 552,
    553, 275, 510, 626, 0,   0,   0,   0,   560, 561, 0,   0,   524, 524, 574, 0,   0,   0,   0,
    521, 0,   0,   0,   524, 524, 0,   587, 0,   0,   590, 0,   593, 0,   521, 521, 0,   0,   0,
    411, 411, 0,   0,   0,   0,   521, 521, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   60,
    61,  62,  63,  64,  65,  66,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  67,  68,  69,  70,  71,  0,   0,   72,  73,  74,  75,  76,  77,  78,  79,  80,
    0,   0,   0,   81,  82,  0,   736, 737, 738, 83,  84,  85,  86,  0,   0,   87,  88,  48,  89,
    90,  0,   0,   0,   0,   91,  92,  93,  94,  0,   0,   0,   95,  96,  97,  98,  99,  100, 101,
    0,   102, 103, 104, 105, 0,   0,   106, 107, 108, 109, 110, 111, 112, 0,   0,   113, 114, 115,
    116, 117, 118, 0,   119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 0,   0,   129, 130, 131,
    132, 133, 134, 135, 136, 137, 648, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 51,  151, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   648, 420, 421, 422, 423, 424, 425, 426, 31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  427, 428, 429, 430,
    431, 0,   0,   432, 433, 434, 435, 436, 437, 438, 439, 440, 0,   0,   0,   441, 442, 0,   0,
    0,   0,   0,   443, 444, 445, 0,   0,   446, 447, 448, 449, 450, 0,   0,   0,   0,   451, 452,
    453, 454, 0,   0,   0,   455, 456, 457, 458, 459, 460, 461, 0,   462, 463, 464, 465, 0,   0,
    466, 467, 468, 469, 470, 471, 472, 0,   0,   473, 474, 475, 476, 477, 478, 0,   479, 480, 481,
    482, 0,   0,   0,   0,   0,   0,   0,   0,   483, 484, 485, 486, 487, 488, 489, 490, 491, 0,
    492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 503, 504, 262, 263, 60,  61,  62,  63,
    64,  65,  66,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    67,  68,  69,  70,  71,  0,   0,   72,  73,  74,  75,  76,  77,  78,  79,  80,  0,   0,   0,
    81,  82,  0,   0,   0,   0,   786, 84,  85,  86,  0,   0,   87,  88,  48,  89,  90,  0,   0,
    0,   0,   91,  92,  93,  94,  0,   0,   0,   95,  96,  97,  98,  99,  100, 101, 0,   102, 103,
    104, 105, 0,   0,   106, 107, 108, 109, 110, 111, 112, 0,   0,   113, 114, 115, 116, 117, 118,
    0,   119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 0,   0,   129, 130, 131, 132, 133, 134,
    135, 136, 137, 0,   138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 51,  420,
    421, 422, 423, 424, 425, 426, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   427, 428, 429, 430, 431, 0,   0,   432, 433, 434, 435, 436, 437, 438, 439, 440,
    0,   0,   0,   441, 442, 0,   0,   0,   0,   0,   443, 444, 445, 0,   0,   446, 447, 0,   449,
    450, 0,   0,   0,   0,   451, 452, 453, 454, 0,   0,   0,   455, 456, 457, 458, 459, 460, 461,
    0,   462, 463, 464, 465, 0,   0,   466, 467, 468, 469, 470, 471, 472, 0,   0,   473, 474, 475,
    476, 477, 478, 0,   479, 480, 481, 482, 0,   0,   0,   0,   0,   0,   0,   0,   483, 484, 485,
    486, 487, 488, 489, 490, 491, 0,   492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 502, 503,
    504, 30,  0,   31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    0,   0,   0,   188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,
    164, 165, 166, 511, 0,   0,   47,  0,   190, 191, 0,   0,   0,   0,   48,  192, 167, 168, 169,
    0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,   0,   0,   0,   0,   0,   49,  0,   50,
    193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   293, 512, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   51,  200,
    201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215,
    188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166,
    744, 0,   0,   0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   527,
    0,   170, 171, 172, 532, 533, 534, 0,   537, 0,   0,   0,   0,   0,   543, 544, 193, 194, 547,
    0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,
    0,   0,   0,   0,   578, 0,   293, 512, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 595,
    596, 0,   0,   0,   0,   0,   0,   0,   0,   0,   606, 607, 0,   0,   0,   200, 201, 202, 203,
    204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 784, 0,   0,
    0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171,
    172, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,
    0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   645, 646, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206,
    207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 782, 0,   0,   0,   0,   190,
    191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,
    195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    293, 512, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 839, 0,   0,   0,   0,   190, 191, 0,   0,
    0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,
    0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   293, 512, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212,
    173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,   161,
    162, 163, 0,   0,   164, 165, 166, 847, 0,   0,   0,   0,   190, 191, 0,   0,   0,   0,   0,
    192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,
    0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   293, 512, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174, 175,
    176, 213, 214, 215, 188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,
    0,   164, 165, 166, 848, 0,   0,   0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 167, 168,
    169, 0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   293, 512, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214,
    215, 188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165,
    166, 849, 0,   0,   0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,
    0,   0,   170, 171, 172, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   193, 194,
    0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   293, 512, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 202,
    203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 0,   0,
    0,   0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170,
    171, 172, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,
    0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   293, 512, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205,
    206, 207, 208, 209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 0,   0,   0,   0,   0,
    190, 191, 0,   0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,
    0,   195, 196, 0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   519, 512, 0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208,
    209, 210, 211, 212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   161, 162, 163, 0,   0,   164, 165, 166, 0,   0,   0,   0,   0,   190, 191, 0,
    0,   0,   0,   0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196,
    0,   0,   0,   0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   645, 646,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211,
    212, 173, 174, 175, 176, 213, 214, 215, 188, 189, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    161, 162, 163, 0,   0,   164, 165, 166, 0,   0,   0,   0,   0,   190, 191, 0,   0,   0,   0,
    0,   192, 167, 168, 169, 0,   0,   0,   0,   170, 171, 172, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,
    0,   0,   0,   197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   198, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 173, 174,
    175, 176, 213, 214, 215, 279, 280, 0,   0,   0,   0,   0,   0,   0,   0,   0,   281, 282, 283,
    0,   0,   284, 285, 286, 0,   0,   0,   0,   0,   190, 191, 0,   0,   0,   0,   0,   192, 287,
    288, 289, 0,   0,   0,   0,   290, 291, 292, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   193, 194, 0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,
    197, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   293, 294, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   199, 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,
    46,  200, 0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 295, 296, 297, 298, 213,
    214, 215, 0,   0,   0,   0,   0,   260, 0,   0,   0,   0,   0,   0,   0,   261, 31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  31,  32,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  0,   0,   0,   0,   0,   0,   0,   0,   0,
    623, 0,   0,   0,   0,   0,   0,   0,   448, 0,   0,   0,   0,   0,   0,   0,   733, 0,   0,
    0,   0,   0,   0,   0,   48,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   262,
    263, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   123, 124, 125, 126,
    127, 128, 0,   0,   0,   0,   0,   0,   0,   0,   0,   262, 263, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   51};

const short ParserGen::yycheck_[] = {
    55,  14,  14,  14,  21,  252, 254, 556, 420, 17,  85,  17,  20,  261, 20,  17,  264, 20,  20,
    261, 432, 261, 264, 261, 264, 261, 264, 114, 264, 47,  48,  49,  444, 445, 272, 586, 261, 113,
    831, 264, 452, 261, 454, 114, 264, 114, 51,  294, 261, 124, 261, 264, 0,   264, 114, 261, 505,
    506, 264, 467, 468, 261, 149, 50,  264, 114, 114, 618, 261, 862, 482, 264, 484, 485, 114, 261,
    81,  51,  264, 113, 492, 493, 494, 495, 496, 497, 498, 499, 500, 501, 261, 51,  504, 264, 261,
    17,  540, 264, 20,  42,  43,  44,  23,  11,  47,  48,  49,  519, 76,  55,  56,  161, 162, 163,
    114, 51,  62,  529, 20,  531, 63,  64,  65,  535, 50,  113, 51,  70,  71,  72,  107, 108, 109,
    110, 111, 112, 548, 113, 156, 85,  86,  113, 554, 555, 113, 51,  558, 113, 94,  95,  113, 113,
    113, 81,  113, 113, 102, 113, 113, 51,  572, 573, 113, 575, 113, 577, 113, 51,  113, 113, 51,
    113, 113, 585, 113, 113, 588, 589, 124, 591, 592, 423, 113, 425, 113, 597, 113, 599, 51,  113,
    602, 603, 604, 605, 113, 141, 745, 113, 51,  51,  51,  51,  51,  42,  43,  51,  51,  619, 47,
    48,  51,  154, 155, 156, 157, 16,  51,  51,  13,  12,  51,  463, 51,  51,  63,  64,  51,  639,
    51,  51,  51,  70,  71,  51,  51,  51,  10,  51,  787, 51,  652, 51,  51,  655, 656, 253, 658,
    253, 51,  51,  253, 253, 664, 665, 51,  51,  668, 505, 506, 51,  672, 508, 51,  505, 506, 505,
    506, 505, 506, 505, 506, 683, 684, 51,  51,  114, 688, 16,  51,  691, 505, 506, 694, 695, 696,
    505, 506, 699, 700, 51,  702, 703, 505, 506, 505, 506, 708, 51,  710, 505, 506, 713, 714, 715,
    716, 505, 506, 261, 14,  14,  264, 51,  505, 506, 51,  759, 51,  51,  730, 505, 506, 51,  734,
    767, 768, 769, 51,  51,  51,  51,  51,  253, 51,  51,  505, 506, 51,  51,  505, 506, 752, 753,
    51,  51,  51,  757, 16,  51,  51,  51,  51,  51,  51,  50,  766, 50,  50,  50,  770, 50,  50,
    773, 774, 775, 16,  777, 421, 422, 55,  56,  16,  51,  427, 428, 429, 62,  431, 51,  790, 50,
    792, 17,  437, 438, 796, 51,  441, 51,  15,  50,  19,  51,  50,  50,  806, 51,  808, 809, 85,
    86,  50,  16,  16,  50,  24,  50,  414, 94,  95,  50,  465, 50,  50,  18,  826, 102, 51,  829,
    50,  436, 50,  21,  439, 440, 479, 480, 51,  51,  51,  51,  842, 25,  51,  51,  50,  490, 491,
    124, 51,  51,  457, 50,  50,  50,  22,  51,  22,  51,  860, 51,  51,  627, 469, 470, 51,  472,
    51,  869, 870, 51,  51,  51,  51,  51,  51,  51,  51,  421, 422, 51,  51,  51,  51,  427, 428,
    429, 51,  431, 51,  51,  51,  51,  51,  437, 438, 502, 541, 441, 421, 422, 51,  51,  252, 746,
    427, 428, 429, 264, 431, 433, 830, 556, 187, 794, 437, 438, 750, 508, 441, 443, 753, 465, 446,
    447, 258, 415, 510, -1,  -1,  -1,  -1,  455, 456, -1,  -1,  479, 480, 461, -1,  -1,  -1,  -1,
    465, -1,  -1,  -1,  490, 491, -1,  473, -1,  -1,  476, -1,  478, -1,  479, 480, -1,  -1,  -1,
    505, 506, -1,  -1,  -1,  -1,  490, 491, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  3,
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    -1,  -1,  -1,  45,  46,  -1,  627, 627, 627, 51,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,
    61,  -1,  -1,  -1,  -1,  66,  67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,
    -1,  81,  82,  83,  84,  -1,  -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,
    99,  100, 101, -1,  103, 104, 105, 106, 107, 108, 109, 110, 111, 112, -1,  -1,  115, 116, 117,
    118, 119, 120, 121, 122, 123, 745, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136,
    137, 138, 139, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  787, 3,   4,   5,   6,   7,   8,   9,   10,
    11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,  -1,  45,  46,  -1,  -1,
    -1,  -1,  -1,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,  61,  -1,  -1,  -1,  -1,  66,  67,
    68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,  82,  83,  84,  -1,  -1,
    87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100, 101, -1,  103, 104, 105,
    106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, 117, 118, 119, 120, 121, 122, 123, -1,
    125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 3,   4,   5,   6,
    7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,  -1,  -1,  -1,
    45,  46,  -1,  -1,  -1,  -1,  51,  52,  53,  54,  -1,  -1,  57,  58,  59,  60,  61,  -1,  -1,
    -1,  -1,  66,  67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,  -1,  81,  82,
    83,  84,  -1,  -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,  99,  100, 101,
    -1,  103, 104, 105, 106, 107, 108, 109, 110, 111, 112, -1,  -1,  115, 116, 117, 118, 119, 120,
    121, 122, 123, -1,  125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 3,
    4,   5,   6,   7,   8,   9,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  26,  27,  28,  29,  30,  -1,  -1,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    -1,  -1,  -1,  45,  46,  -1,  -1,  -1,  -1,  -1,  52,  53,  54,  -1,  -1,  57,  58,  -1,  60,
    61,  -1,  -1,  -1,  -1,  66,  67,  68,  69,  -1,  -1,  -1,  73,  74,  75,  76,  77,  78,  79,
    -1,  81,  82,  83,  84,  -1,  -1,  87,  88,  89,  90,  91,  92,  93,  -1,  -1,  96,  97,  98,
    99,  100, 101, -1,  103, 104, 105, 106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, 117,
    118, 119, 120, 121, 122, 123, -1,  125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136,
    137, 8,   -1,  10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    -1,  -1,  -1,  31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,
    47,  48,  49,  50,  -1,  -1,  51,  -1,  55,  56,  -1,  -1,  -1,  -1,  59,  62,  63,  64,  65,
    -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  80,  -1,  82,
    85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138, 141,
    142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160,
    31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,
    50,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  422,
    -1,  70,  71,  72,  427, 428, 429, -1,  431, -1,  -1,  -1,  -1,  -1,  437, 438, 85,  86,  441,
    -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  465, -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, 479,
    480, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  490, 491, -1,  -1,  -1,  141, 142, 143, 144,
    145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 31,  32,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  50,  -1,  -1,
    -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,  64,  65,  -1,  -1,  -1,  -1,  70,  71,
    72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147,
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
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,  -1,  -1,  47,  48,  49,  -1,  -1,
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
    -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  114, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  124, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  42,  43,  44,
    -1,  -1,  47,  48,  49,  -1,  -1,  -1,  -1,  -1,  55,  56,  -1,  -1,  -1,  -1,  -1,  62,  63,
    64,  65,  -1,  -1,  -1,  -1,  70,  71,  72,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  95,  -1,  -1,  -1,  -1,  -1,  -1,
    102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  113, 114, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  124, 10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
    25,  141, -1,  -1,  144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, -1,  -1,  -1,  -1,  -1,  51,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  59,  10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    51,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  59,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  51,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  59,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138,
    139, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  107, 108, 109, 110,
    111, 112, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138, 139, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  138};

const short ParserGen::yystos_[] = {
    0,   161, 162, 163, 356, 113, 210, 114, 340, 114, 350, 0,   114, 211, 341, 351, 107, 108, 109,
    110, 111, 112, 212, 213, 214, 215, 216, 217, 218, 50,  8,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  25,  51,  59,  80,  82,  138, 169, 172, 174, 177, 182,
    345, 346, 349, 3,   4,   5,   6,   7,   8,   9,   26,  27,  28,  29,  30,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  45,  46,  51,  52,  53,  54,  57,  58,  60,  61,  66,  67,  68,  69,
    73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  87,  88,  89,  90,  91,  92,  93,  96,
    97,  98,  99,  100, 101, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139,
    168, 169, 171, 172, 173, 174, 176, 355, 114, 42,  43,  44,  47,  48,  49,  63,  64,  65,  70,
    71,  72,  154, 155, 156, 157, 187, 189, 190, 191, 224, 114, 114, 224, 357, 358, 51,  31,  32,
    55,  56,  62,  85,  86,  94,  95,  102, 114, 124, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 158, 159, 160, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 195,
    196, 197, 198, 199, 200, 201, 202, 203, 204, 209, 343, 113, 42,  43,  47,  48,  63,  64,  70,
    71,  114, 352, 353, 51,  219, 23,  11,  114, 211, 342, 344, 76,  51,  59,  138, 139, 165, 166,
    170, 178, 182, 224, 195, 51,  81,  347, 348, 340, 85,  124, 354, 31,  32,  42,  43,  44,  47,
    48,  49,  63,  64,  65,  70,  71,  72,  113, 114, 154, 155, 156, 157, 183, 184, 185, 186, 188,
    192, 193, 195, 197, 198, 199, 201, 202, 203, 221, 222, 223, 226, 229, 230, 231, 234, 235, 236,
    237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 255, 256,
    257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275,
    276, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294,
    295, 296, 297, 298, 299, 300, 301, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322,
    323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 337, 221, 51,  20,  114, 203, 50,
    51,  308, 3,   4,   5,   6,   7,   8,   9,   26,  27,  28,  29,  30,  33,  34,  35,  36,  37,
    38,  39,  40,  41,  45,  46,  52,  53,  54,  57,  58,  59,  60,  61,  66,  67,  68,  69,  73,
    74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  87,  88,  89,  90,  91,  92,  93,  96,  97,
    98,  99,  100, 101, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 123, 125, 126,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 165, 175, 179, 220, 190, 342, 50,  114,
    204, 225, 231, 232, 337, 225, 113, 204, 232, 335, 336, 337, 338, 339, 339, 231, 113, 231, 113,
    339, 339, 339, 113, 227, 339, 225, 227, 113, 113, 357, 339, 339, 357, 357, 339, 113, 227, 225,
    225, 227, 227, 113, 113, 113, 225, 113, 225, 227, 227, 357, 55,  56,  62,  85,  86,  94,  95,
    102, 124, 113, 113, 227, 113, 231, 113, 339, 254, 357, 254, 254, 357, 357, 113, 357, 227, 113,
    113, 227, 113, 113, 227, 228, 339, 339, 113, 225, 113, 225, 225, 113, 113, 113, 113, 339, 339,
    225, 225, 225, 225, 225, 225, 225, 225, 225, 225, 357, 113, 225, 222, 222, 51,  179, 51,  347,
    233, 51,  225, 51,  51,  51,  225, 51,  225, 51,  51,  51,  225, 51,  51,  51,  51,  308, 113,
    114, 194, 204, 205, 206, 207, 16,  51,  51,  13,  12,  51,  225, 51,  51,  51,  51,  51,  225,
    225, 194, 51,  225, 51,  51,  51,  10,  306, 51,  51,  51,  51,  51,  51,  51,  51,  51,  225,
    225, 51,  225, 51,  225, 51,  51,  16,  51,  51,  14,  14,  225, 306, 51,  225, 225, 51,  225,
    225, 51,  51,  51,  51,  225, 51,  225, 51,  51,  225, 225, 225, 225, 51,  51,  51,  51,  51,
    51,  51,  51,  51,  51,  51,  51,  306, 225, 51,  51,  51,  167, 168, 169, 172, 174, 180, 50,
    50,  50,  225, 50,  309, 208, 50,  225, 225, 225, 225, 225, 225, 50,  225, 225, 16,  225, 225,
    50,  225, 225, 225, 225, 225, 16,  225, 225, 225, 225, 225, 225, 225, 225, 225, 225, 16,  225,
    225, 51,  51,  50,  51,  50,  194, 51,  176, 181, 51,  17,  302, 15,  304, 304, 50,  225, 310,
    310, 51,  50,  225, 50,  308, 51,  50,  19,  307, 16,  16,  50,  225, 308, 308, 308, 225, 50,
    50,  225, 225, 225, 50,  225, 50,  194, 225, 18,  303, 225, 24,  305, 305, 51,  225, 50,  50,
    51,  51,  51,  50,  51,  225, 21,  225, 225, 51,  51,  50,  50,  50,  50,  51,  51,  50,  50,
    50,  51,  51,  51,  225, 25,  225, 302, 303, 51,  51,  51,  51,  225, 22,  22,  51,  51,  51,
    51,  51,  51,  51,  51,  225, 303, 51,  51,  225, 225, 51,  51,  51,  51,  51,  51,  51,  51,
    51};

const short ParserGen::yyr1_[] = {
    0,   164, 356, 356, 356, 210, 211, 211, 358, 357, 212, 212, 212, 212, 212, 212, 218, 213, 214,
    224, 224, 224, 224, 215, 216, 217, 219, 219, 178, 178, 221, 222, 222, 222, 222, 222, 222, 222,
    222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222, 222,
    222, 222, 222, 222, 222, 222, 222, 222, 165, 166, 166, 166, 223, 220, 220, 179, 179, 340, 341,
    341, 345, 345, 343, 343, 342, 342, 347, 348, 348, 346, 349, 349, 349, 344, 344, 177, 177, 177,
    172, 168, 168, 168, 168, 168, 168, 169, 170, 182, 182, 182, 182, 182, 182, 182, 182, 182, 182,
    182, 182, 182, 182, 182, 182, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171,
    171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 171, 195, 195, 195, 195, 195,
    195, 195, 195, 195, 195, 196, 209, 197, 198, 199, 201, 202, 203, 183, 184, 185, 186, 188, 192,
    193, 187, 187, 187, 187, 189, 189, 189, 189, 190, 190, 190, 190, 191, 191, 191, 191, 200, 200,
    204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204, 204,
    204, 204, 308, 308, 225, 225, 225, 225, 335, 335, 336, 336, 337, 337, 337, 337, 337, 337, 337,
    337, 337, 337, 227, 228, 226, 226, 229, 230, 230, 231, 338, 339, 339, 232, 233, 233, 180, 167,
    167, 167, 167, 174, 175, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234,
    234, 234, 235, 235, 235, 235, 235, 235, 235, 235, 235, 319, 319, 319, 319, 319, 319, 319, 319,
    319, 319, 319, 319, 319, 319, 319, 236, 332, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296,
    297, 298, 299, 300, 301, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 333, 334,
    237, 237, 237, 238, 239, 240, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244, 244,
    244, 244, 244, 244, 244, 244, 244, 244, 244, 245, 304, 304, 305, 305, 246, 247, 310, 310, 310,
    248, 249, 306, 306, 250, 257, 267, 307, 307, 254, 251, 252, 253, 255, 256, 258, 259, 260, 261,
    262, 263, 264, 265, 266, 354, 354, 352, 350, 351, 351, 353, 353, 353, 353, 353, 353, 353, 353,
    173, 173, 355, 355, 311, 311, 311, 311, 311, 311, 311, 312, 313, 314, 315, 316, 317, 318, 241,
    241, 242, 243, 194, 194, 205, 205, 206, 309, 309, 207, 208, 208, 181, 176, 176, 176, 176, 176,
    268, 268, 268, 268, 268, 268, 268, 269, 270, 271, 272, 273, 274, 275, 276, 276, 276, 276, 276,
    276, 276, 276, 276, 276, 302, 302, 303, 303, 277, 278, 279, 280, 281, 282, 283, 284, 285, 286};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 3, 0, 4, 0, 2, 1, 1,  1,  1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2,  2,  4, 0, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 2,  2,  2, 3, 0, 2, 2, 1, 1, 3, 0, 2, 1,  2,  5, 5, 1, 1, 1,
    0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 0, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 4, 5, 1, 1, 1, 4,  4,  3, 3, 1, 1, 3,
    0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  4, 4, 4, 4, 4,
    4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7,  4,  4, 4, 7, 4, 7,
    8, 7, 7, 4, 7, 7, 4, 4, 4, 4, 4, 4,  4,  4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 4,  4,  6, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2, 8, 8,
    0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4, 1, 1,  4,  3, 0, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1,  1,  1, 1, 1, 1, 6, 6, 4, 8, 8, 4, 8,  1,  1, 6, 6, 1, 1,
    1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4,  4,  4, 4, 4, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2,  11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


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
    1923, 1926, 1929, 1932, 1935, 1938, 1941, 1944, 1949, 1951, 1961, 1963, 1969, 1969, 1969, 1969,
    1969, 1969, 1970, 1974, 1980, 1986, 1993, 2004, 2015, 2022, 2033, 2033, 2037, 2044, 2051, 2051,
    2055, 2055, 2059, 2065, 2066, 2073, 2079, 2082, 2089, 2096, 2097, 2098, 2099, 2100, 2103, 2103,
    2103, 2103, 2103, 2103, 2103, 2105, 2110, 2115, 2120, 2125, 2130, 2135, 2141, 2142, 2143, 2144,
    2145, 2146, 2147, 2148, 2149, 2150, 2155, 2158, 2165, 2168, 2174, 2184, 2189, 2194, 2199, 2204,
    2209, 2214, 2219, 2224};

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
#line 7628 "parser_gen.cpp"

#line 2228 "grammar.yy"
