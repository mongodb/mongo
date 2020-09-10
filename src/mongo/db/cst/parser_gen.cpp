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
#line 354 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2008 "parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH match
#line 357 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2016 "parser_gen.cpp"
                    break;

                    case 4:  // start: START_SORT sortSpecs
#line 360 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2024 "parser_gen.cpp"
                    break;

                    case 5:  // pipeline: "array" stageList "end of array"
#line 367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2032 "parser_gen.cpp"
                    break;

                    case 6:  // stageList: %empty
#line 373 "grammar.yy"
                    {
                    }
#line 2038 "parser_gen.cpp"
                    break;

                    case 7:  // stageList: "object" stage "end of object" stageList
#line 374 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 2046 "parser_gen.cpp"
                    break;

                    case 8:  // $@1: %empty
#line 382 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 2052 "parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2058 "parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2064 "parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2070 "parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2076 "parser_gen.cpp"
                    break;

                    case 14:  // stage: project
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2082 "parser_gen.cpp"
                    break;

                    case 15:  // stage: sample
#line 385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2088 "parser_gen.cpp"
                    break;

                    case 16:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 388 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 2100 "parser_gen.cpp"
                    break;

                    case 17:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 398 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 2108 "parser_gen.cpp"
                    break;

                    case 18:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 404 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 2121 "parser_gen.cpp"
                    break;

                    case 19:  // num: int
#line 414 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2127 "parser_gen.cpp"
                    break;

                    case 20:  // num: long
#line 414 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2133 "parser_gen.cpp"
                    break;

                    case 21:  // num: double
#line 414 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2139 "parser_gen.cpp"
                    break;

                    case 22:  // num: decimal
#line 414 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2145 "parser_gen.cpp"
                    break;

                    case 23:  // skip: STAGE_SKIP num
#line 418 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2153 "parser_gen.cpp"
                    break;

                    case 24:  // limit: STAGE_LIMIT num
#line 423 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2161 "parser_gen.cpp"
                    break;

                    case 25:  // project: STAGE_PROJECT "object" projectFields "end of object"
#line 428 "grammar.yy"
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
#line 2182 "parser_gen.cpp"
                    break;

                    case 26:  // projectFields: %empty
#line 447 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2190 "parser_gen.cpp"
                    break;

                    case 27:  // projectFields: projectFields projectField
#line 450 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2199 "parser_gen.cpp"
                    break;

                    case 28:  // projectField: ID topLevelProjection
#line 457 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2207 "parser_gen.cpp"
                    break;

                    case 29:  // projectField: aggregationProjectionFieldname topLevelProjection
#line 460 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2215 "parser_gen.cpp"
                    break;

                    case 30:  // topLevelProjection: projection
#line 466 "grammar.yy"
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
#line 2231 "parser_gen.cpp"
                    break;

                    case 31:  // projection: string
#line 480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2237 "parser_gen.cpp"
                    break;

                    case 32:  // projection: binary
#line 481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2243 "parser_gen.cpp"
                    break;

                    case 33:  // projection: undefined
#line 482 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2249 "parser_gen.cpp"
                    break;

                    case 34:  // projection: objectId
#line 483 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2255 "parser_gen.cpp"
                    break;

                    case 35:  // projection: date
#line 484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2261 "parser_gen.cpp"
                    break;

                    case 36:  // projection: null
#line 485 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2267 "parser_gen.cpp"
                    break;

                    case 37:  // projection: regex
#line 486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2273 "parser_gen.cpp"
                    break;

                    case 38:  // projection: dbPointer
#line 487 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2279 "parser_gen.cpp"
                    break;

                    case 39:  // projection: javascript
#line 488 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2285 "parser_gen.cpp"
                    break;

                    case 40:  // projection: symbol
#line 489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2291 "parser_gen.cpp"
                    break;

                    case 41:  // projection: javascriptWScope
#line 490 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2297 "parser_gen.cpp"
                    break;

                    case 42:  // projection: "1 (int)"
#line 491 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2305 "parser_gen.cpp"
                    break;

                    case 43:  // projection: "-1 (int)"
#line 494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2313 "parser_gen.cpp"
                    break;

                    case 44:  // projection: "arbitrary integer"
#line 497 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2321 "parser_gen.cpp"
                    break;

                    case 45:  // projection: "zero (int)"
#line 500 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2329 "parser_gen.cpp"
                    break;

                    case 46:  // projection: "1 (long)"
#line 503 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2337 "parser_gen.cpp"
                    break;

                    case 47:  // projection: "-1 (long)"
#line 506 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2345 "parser_gen.cpp"
                    break;

                    case 48:  // projection: "arbitrary long"
#line 509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2353 "parser_gen.cpp"
                    break;

                    case 49:  // projection: "zero (long)"
#line 512 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2361 "parser_gen.cpp"
                    break;

                    case 50:  // projection: "1 (double)"
#line 515 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2369 "parser_gen.cpp"
                    break;

                    case 51:  // projection: "-1 (double)"
#line 518 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2377 "parser_gen.cpp"
                    break;

                    case 52:  // projection: "arbitrary double"
#line 521 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2385 "parser_gen.cpp"
                    break;

                    case 53:  // projection: "zero (double)"
#line 524 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2393 "parser_gen.cpp"
                    break;

                    case 54:  // projection: "1 (decimal)"
#line 527 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2401 "parser_gen.cpp"
                    break;

                    case 55:  // projection: "-1 (decimal)"
#line 530 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2409 "parser_gen.cpp"
                    break;

                    case 56:  // projection: "arbitrary decimal"
#line 533 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2417 "parser_gen.cpp"
                    break;

                    case 57:  // projection: "zero (decimal)"
#line 536 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2425 "parser_gen.cpp"
                    break;

                    case 58:  // projection: "true"
#line 539 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2433 "parser_gen.cpp"
                    break;

                    case 59:  // projection: "false"
#line 542 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2441 "parser_gen.cpp"
                    break;

                    case 60:  // projection: timestamp
#line 545 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2447 "parser_gen.cpp"
                    break;

                    case 61:  // projection: minKey
#line 546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2453 "parser_gen.cpp"
                    break;

                    case 62:  // projection: maxKey
#line 547 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2459 "parser_gen.cpp"
                    break;

                    case 63:  // projection: projectionObject
#line 548 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2465 "parser_gen.cpp"
                    break;

                    case 64:  // projection: compoundNonObjectExpression
#line 549 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2471 "parser_gen.cpp"
                    break;

                    case 65:  // aggregationProjectionFieldname: projectionFieldname
#line 554 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2481 "parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: "fieldname"
#line 562 "grammar.yy"
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
#line 2499 "parser_gen.cpp"
                    break;

                    case 67:  // projectionFieldname: argAsProjectionPath
#line 575 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2505 "parser_gen.cpp"
                    break;

                    case 68:  // projectionFieldname: "fieldname containing dotted path"
#line 576 "grammar.yy"
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
#line 2523 "parser_gen.cpp"
                    break;

                    case 69:  // projectionObject: "object" projectionObjectFields "end of object"
#line 593 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2531 "parser_gen.cpp"
                    break;

                    case 70:  // projectionObjectFields: projectionObjectField
#line 600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2540 "parser_gen.cpp"
                    break;

                    case 71:  // projectionObjectFields: projectionObjectFields
                              // projectionObjectField
#line 604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2549 "parser_gen.cpp"
                    break;

                    case 72:  // projectionObjectField: idAsProjectionPath projection
#line 612 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2557 "parser_gen.cpp"
                    break;

                    case 73:  // projectionObjectField: aggregationProjectionFieldname projection
#line 615 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2565 "parser_gen.cpp"
                    break;

                    case 74:  // match: "object" predicates "end of object"
#line 621 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2573 "parser_gen.cpp"
                    break;

                    case 75:  // predicates: %empty
#line 627 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2581 "parser_gen.cpp"
                    break;

                    case 76:  // predicates: predicates predicate
#line 630 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2590 "parser_gen.cpp"
                    break;

                    case 77:  // predicate: predFieldname predValue
#line 636 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2598 "parser_gen.cpp"
                    break;

                    case 78:  // predicate: logicalExpr
#line 639 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2604 "parser_gen.cpp"
                    break;

                    case 79:  // predicate: commentExpr
#line 640 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2610 "parser_gen.cpp"
                    break;

                    case 80:  // predValue: simpleValue
#line 647 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2616 "parser_gen.cpp"
                    break;

                    case 81:  // predValue: "object" compoundMatchExprs "end of object"
#line 648 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2624 "parser_gen.cpp"
                    break;

                    case 82:  // compoundMatchExprs: %empty
#line 654 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2632 "parser_gen.cpp"
                    break;

                    case 83:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 657 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2641 "parser_gen.cpp"
                    break;

                    case 84:  // operatorExpression: notExpr
#line 665 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2647 "parser_gen.cpp"
                    break;

                    case 85:  // operatorExpression: existsExpr
#line 665 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2653 "parser_gen.cpp"
                    break;

                    case 86:  // operatorExpression: typeExpr
#line 665 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2659 "parser_gen.cpp"
                    break;

                    case 87:  // existsExpr: EXISTS value
#line 669 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::existsExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2667 "parser_gen.cpp"
                    break;

                    case 88:  // typeArray: "array" typeValues "end of array"
#line 675 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 2675 "parser_gen.cpp"
                    break;

                    case 89:  // typeValues: %empty
#line 681 "grammar.yy"
                    {
                    }
#line 2681 "parser_gen.cpp"
                    break;

                    case 90:  // typeValues: typeValues typeValue
#line 682 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2690 "parser_gen.cpp"
                    break;

                    case 91:  // typeValue: num
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2696 "parser_gen.cpp"
                    break;

                    case 92:  // typeValue: string
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2702 "parser_gen.cpp"
                    break;

                    case 93:  // typeExpr: TYPE typeValue
#line 693 "grammar.yy"
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
#line 2716 "parser_gen.cpp"
                    break;

                    case 94:  // typeExpr: TYPE typeArray
#line 702 "grammar.yy"
                    {
                        auto&& types = YY_MOVE(yystack_[0].value.as<CNode>());
                        if (auto status = c_node_validation::validateTypeOperatorArgument(types);
                            !status.isOK()) {
                            error(yystack_[1].location, status.reason());
                        }
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::type, std::move(types)};
                    }
#line 2728 "parser_gen.cpp"
                    break;

                    case 95:  // commentExpr: COMMENT value
#line 712 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::commentExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2736 "parser_gen.cpp"
                    break;

                    case 96:  // notExpr: NOT regex
#line 718 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2744 "parser_gen.cpp"
                    break;

                    case 97:  // notExpr: NOT "object" compoundMatchExprs operatorExpression "end of
                              // object"
#line 723 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[2].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2755 "parser_gen.cpp"
                    break;

                    case 98:  // logicalExpr: logicalExprField "array" additionalExprs match "end of
                              // array"
#line 733 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[2].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2765 "parser_gen.cpp"
                    break;

                    case 99:  // logicalExprField: AND
#line 741 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2771 "parser_gen.cpp"
                    break;

                    case 100:  // logicalExprField: OR
#line 742 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2777 "parser_gen.cpp"
                    break;

                    case 101:  // logicalExprField: NOR
#line 743 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2783 "parser_gen.cpp"
                    break;

                    case 102:  // additionalExprs: %empty
#line 746 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2791 "parser_gen.cpp"
                    break;

                    case 103:  // additionalExprs: additionalExprs match
#line 749 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2800 "parser_gen.cpp"
                    break;

                    case 104:  // predFieldname: idAsUserFieldname
#line 756 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2806 "parser_gen.cpp"
                    break;

                    case 105:  // predFieldname: argAsUserFieldname
#line 756 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2812 "parser_gen.cpp"
                    break;

                    case 106:  // predFieldname: invariableUserFieldname
#line 756 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2818 "parser_gen.cpp"
                    break;

                    case 107:  // invariableUserFieldname: "fieldname"
#line 759 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2826 "parser_gen.cpp"
                    break;

                    case 108:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 767 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2834 "parser_gen.cpp"
                    break;

                    case 109:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 770 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2842 "parser_gen.cpp"
                    break;

                    case 110:  // stageAsUserFieldname: STAGE_SKIP
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2850 "parser_gen.cpp"
                    break;

                    case 111:  // stageAsUserFieldname: STAGE_LIMIT
#line 776 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2858 "parser_gen.cpp"
                    break;

                    case 112:  // stageAsUserFieldname: STAGE_PROJECT
#line 779 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2866 "parser_gen.cpp"
                    break;

                    case 113:  // stageAsUserFieldname: STAGE_SAMPLE
#line 782 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2874 "parser_gen.cpp"
                    break;

                    case 114:  // argAsUserFieldname: arg
#line 788 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2882 "parser_gen.cpp"
                    break;

                    case 115:  // argAsProjectionPath: arg
#line 794 "grammar.yy"
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
#line 2900 "parser_gen.cpp"
                    break;

                    case 116:  // arg: "coll argument"
#line 813 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 2908 "parser_gen.cpp"
                    break;

                    case 117:  // arg: "pipeline argument"
#line 816 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 2916 "parser_gen.cpp"
                    break;

                    case 118:  // arg: "size argument"
#line 819 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 2924 "parser_gen.cpp"
                    break;

                    case 119:  // arg: "input argument"
#line 822 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 2932 "parser_gen.cpp"
                    break;

                    case 120:  // arg: "to argument"
#line 825 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 2940 "parser_gen.cpp"
                    break;

                    case 121:  // arg: "onError argument"
#line 828 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 2948 "parser_gen.cpp"
                    break;

                    case 122:  // arg: "onNull argument"
#line 831 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 2956 "parser_gen.cpp"
                    break;

                    case 123:  // arg: "dateString argument"
#line 834 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 2964 "parser_gen.cpp"
                    break;

                    case 124:  // arg: "format argument"
#line 837 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 2972 "parser_gen.cpp"
                    break;

                    case 125:  // arg: "timezone argument"
#line 840 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 2980 "parser_gen.cpp"
                    break;

                    case 126:  // arg: "date argument"
#line 843 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 2988 "parser_gen.cpp"
                    break;

                    case 127:  // arg: "chars argument"
#line 846 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 2996 "parser_gen.cpp"
                    break;

                    case 128:  // arg: "regex argument"
#line 849 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 3004 "parser_gen.cpp"
                    break;

                    case 129:  // arg: "options argument"
#line 852 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 3012 "parser_gen.cpp"
                    break;

                    case 130:  // arg: "find argument"
#line 855 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 3020 "parser_gen.cpp"
                    break;

                    case 131:  // arg: "replacement argument"
#line 858 "grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 3028 "parser_gen.cpp"
                    break;

                    case 132:  // aggExprAsUserFieldname: ADD
#line 866 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 3036 "parser_gen.cpp"
                    break;

                    case 133:  // aggExprAsUserFieldname: ATAN2
#line 869 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 3044 "parser_gen.cpp"
                    break;

                    case 134:  // aggExprAsUserFieldname: AND
#line 872 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 3052 "parser_gen.cpp"
                    break;

                    case 135:  // aggExprAsUserFieldname: CONST_EXPR
#line 875 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 3060 "parser_gen.cpp"
                    break;

                    case 136:  // aggExprAsUserFieldname: LITERAL
#line 878 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 3068 "parser_gen.cpp"
                    break;

                    case 137:  // aggExprAsUserFieldname: OR
#line 881 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 3076 "parser_gen.cpp"
                    break;

                    case 138:  // aggExprAsUserFieldname: NOT
#line 884 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 3084 "parser_gen.cpp"
                    break;

                    case 139:  // aggExprAsUserFieldname: CMP
#line 887 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 3092 "parser_gen.cpp"
                    break;

                    case 140:  // aggExprAsUserFieldname: EQ
#line 890 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 3100 "parser_gen.cpp"
                    break;

                    case 141:  // aggExprAsUserFieldname: GT
#line 893 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 3108 "parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: GTE
#line 896 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 3116 "parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: LT
#line 899 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 3124 "parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: LTE
#line 902 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 3132 "parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: NE
#line 905 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 3140 "parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: CONVERT
#line 908 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 3148 "parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: TO_BOOL
#line 911 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 3156 "parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: TO_DATE
#line 914 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 3164 "parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: TO_DECIMAL
#line 917 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 3172 "parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: TO_DOUBLE
#line 920 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 3180 "parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: TO_INT
#line 923 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 3188 "parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: TO_LONG
#line 926 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 3196 "parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 929 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 3204 "parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: TO_STRING
#line 932 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 3212 "parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: TYPE
#line 935 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 3220 "parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: ABS
#line 938 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3228 "parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: CEIL
#line 941 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3236 "parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: DIVIDE
#line 944 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3244 "parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: EXPONENT
#line 947 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3252 "parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: FLOOR
#line 950 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3260 "parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: LN
#line 953 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3268 "parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: LOG
#line 956 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3276 "parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: LOGTEN
#line 959 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3284 "parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: MOD
#line 962 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3292 "parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: MULTIPLY
#line 965 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3300 "parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: POW
#line 968 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3308 "parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: ROUND
#line 971 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3316 "parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: "slice"
#line 974 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3324 "parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: SQRT
#line 977 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3332 "parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: SUBTRACT
#line 980 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3340 "parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: TRUNC
#line 983 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3348 "parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: CONCAT
#line 986 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3356 "parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 989 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3364 "parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 992 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3372 "parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 995 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3380 "parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 998 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3388 "parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: LTRIM
#line 1001 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3396 "parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: META
#line 1004 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3404 "parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: REGEX_FIND
#line 1007 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3412 "parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 1010 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3420 "parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: REGEX_MATCH
#line 1013 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3428 "parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: REPLACE_ONE
#line 1016 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3436 "parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: REPLACE_ALL
#line 1019 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3444 "parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: RTRIM
#line 1022 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3452 "parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: SPLIT
#line 1025 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3460 "parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 1028 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3468 "parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: STR_LEN_CP
#line 1031 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3476 "parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 1034 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3484 "parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: SUBSTR
#line 1037 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3492 "parser_gen.cpp"
                    break;

                    case 190:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 1040 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3500 "parser_gen.cpp"
                    break;

                    case 191:  // aggExprAsUserFieldname: SUBSTR_CP
#line 1043 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3508 "parser_gen.cpp"
                    break;

                    case 192:  // aggExprAsUserFieldname: TO_LOWER
#line 1046 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3516 "parser_gen.cpp"
                    break;

                    case 193:  // aggExprAsUserFieldname: TRIM
#line 1049 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3524 "parser_gen.cpp"
                    break;

                    case 194:  // aggExprAsUserFieldname: TO_UPPER
#line 1052 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3532 "parser_gen.cpp"
                    break;

                    case 195:  // aggExprAsUserFieldname: "allElementsTrue"
#line 1055 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3540 "parser_gen.cpp"
                    break;

                    case 196:  // aggExprAsUserFieldname: "anyElementTrue"
#line 1058 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3548 "parser_gen.cpp"
                    break;

                    case 197:  // aggExprAsUserFieldname: "setDifference"
#line 1061 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3556 "parser_gen.cpp"
                    break;

                    case 198:  // aggExprAsUserFieldname: "setEquals"
#line 1064 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3564 "parser_gen.cpp"
                    break;

                    case 199:  // aggExprAsUserFieldname: "setIntersection"
#line 1067 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3572 "parser_gen.cpp"
                    break;

                    case 200:  // aggExprAsUserFieldname: "setIsSubset"
#line 1070 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3580 "parser_gen.cpp"
                    break;

                    case 201:  // aggExprAsUserFieldname: "setUnion"
#line 1073 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3588 "parser_gen.cpp"
                    break;

                    case 202:  // aggExprAsUserFieldname: SIN
#line 1076 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sin"};
                    }
#line 3596 "parser_gen.cpp"
                    break;

                    case 203:  // aggExprAsUserFieldname: COS
#line 1079 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cos"};
                    }
#line 3604 "parser_gen.cpp"
                    break;

                    case 204:  // aggExprAsUserFieldname: TAN
#line 1082 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tan"};
                    }
#line 3612 "parser_gen.cpp"
                    break;

                    case 205:  // aggExprAsUserFieldname: SINH
#line 1085 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sinh"};
                    }
#line 3620 "parser_gen.cpp"
                    break;

                    case 206:  // aggExprAsUserFieldname: COSH
#line 1088 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cosh"};
                    }
#line 3628 "parser_gen.cpp"
                    break;

                    case 207:  // aggExprAsUserFieldname: TANH
#line 1091 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$tanh"};
                    }
#line 3636 "parser_gen.cpp"
                    break;

                    case 208:  // aggExprAsUserFieldname: ASIN
#line 1094 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asin"};
                    }
#line 3644 "parser_gen.cpp"
                    break;

                    case 209:  // aggExprAsUserFieldname: ACOS
#line 1097 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acos"};
                    }
#line 3652 "parser_gen.cpp"
                    break;

                    case 210:  // aggExprAsUserFieldname: ATAN
#line 1100 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan"};
                    }
#line 3660 "parser_gen.cpp"
                    break;

                    case 211:  // aggExprAsUserFieldname: ASINH
#line 1103 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$asinh"};
                    }
#line 3668 "parser_gen.cpp"
                    break;

                    case 212:  // aggExprAsUserFieldname: ACOSH
#line 1106 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$acosh"};
                    }
#line 3676 "parser_gen.cpp"
                    break;

                    case 213:  // aggExprAsUserFieldname: ATANH
#line 1109 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atanh"};
                    }
#line 3684 "parser_gen.cpp"
                    break;

                    case 214:  // aggExprAsUserFieldname: DEGREES_TO_RADIANS
#line 1112 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$degreesToRadians"};
                    }
#line 3692 "parser_gen.cpp"
                    break;

                    case 215:  // aggExprAsUserFieldname: RADIANS_TO_DEGREES
#line 1115 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$radiansToDegrees"};
                    }
#line 3700 "parser_gen.cpp"
                    break;

                    case 216:  // string: "string"
#line 1122 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3708 "parser_gen.cpp"
                    break;

                    case 217:  // string: "geoNearDistance"
#line 1127 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 3716 "parser_gen.cpp"
                    break;

                    case 218:  // string: "geoNearPoint"
#line 1130 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 3724 "parser_gen.cpp"
                    break;

                    case 219:  // string: "indexKey"
#line 1133 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 3732 "parser_gen.cpp"
                    break;

                    case 220:  // string: "randVal"
#line 1136 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3740 "parser_gen.cpp"
                    break;

                    case 221:  // string: "recordId"
#line 1139 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 3748 "parser_gen.cpp"
                    break;

                    case 222:  // string: "searchHighlights"
#line 1142 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 3756 "parser_gen.cpp"
                    break;

                    case 223:  // string: "searchScore"
#line 1145 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 3764 "parser_gen.cpp"
                    break;

                    case 224:  // string: "sortKey"
#line 1148 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 3772 "parser_gen.cpp"
                    break;

                    case 225:  // string: "textScore"
#line 1151 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3780 "parser_gen.cpp"
                    break;

                    case 226:  // aggregationFieldPath: "$-prefixed string"
#line 1157 "grammar.yy"
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
#line 3796 "parser_gen.cpp"
                    break;

                    case 227:  // variable: "$$-prefixed string"
#line 1171 "grammar.yy"
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
#line 3812 "parser_gen.cpp"
                    break;

                    case 228:  // binary: "BinData"
#line 1185 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3820 "parser_gen.cpp"
                    break;

                    case 229:  // undefined: "undefined"
#line 1191 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3828 "parser_gen.cpp"
                    break;

                    case 230:  // objectId: "ObjectID"
#line 1197 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3836 "parser_gen.cpp"
                    break;

                    case 231:  // date: "Date"
#line 1203 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3844 "parser_gen.cpp"
                    break;

                    case 232:  // null: "null"
#line 1209 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3852 "parser_gen.cpp"
                    break;

                    case 233:  // regex: "regex"
#line 1215 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3860 "parser_gen.cpp"
                    break;

                    case 234:  // dbPointer: "dbPointer"
#line 1221 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3868 "parser_gen.cpp"
                    break;

                    case 235:  // javascript: "Code"
#line 1227 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3876 "parser_gen.cpp"
                    break;

                    case 236:  // symbol: "Symbol"
#line 1233 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3884 "parser_gen.cpp"
                    break;

                    case 237:  // javascriptWScope: "CodeWScope"
#line 1239 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3892 "parser_gen.cpp"
                    break;

                    case 238:  // timestamp: "Timestamp"
#line 1245 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3900 "parser_gen.cpp"
                    break;

                    case 239:  // minKey: "minKey"
#line 1251 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3908 "parser_gen.cpp"
                    break;

                    case 240:  // maxKey: "maxKey"
#line 1257 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3916 "parser_gen.cpp"
                    break;

                    case 241:  // int: "arbitrary integer"
#line 1263 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3924 "parser_gen.cpp"
                    break;

                    case 242:  // int: "zero (int)"
#line 1266 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3932 "parser_gen.cpp"
                    break;

                    case 243:  // int: "1 (int)"
#line 1269 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3940 "parser_gen.cpp"
                    break;

                    case 244:  // int: "-1 (int)"
#line 1272 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3948 "parser_gen.cpp"
                    break;

                    case 245:  // long: "arbitrary long"
#line 1278 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3956 "parser_gen.cpp"
                    break;

                    case 246:  // long: "zero (long)"
#line 1281 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3964 "parser_gen.cpp"
                    break;

                    case 247:  // long: "1 (long)"
#line 1284 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3972 "parser_gen.cpp"
                    break;

                    case 248:  // long: "-1 (long)"
#line 1287 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3980 "parser_gen.cpp"
                    break;

                    case 249:  // double: "arbitrary double"
#line 1293 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3988 "parser_gen.cpp"
                    break;

                    case 250:  // double: "zero (double)"
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3996 "parser_gen.cpp"
                    break;

                    case 251:  // double: "1 (double)"
#line 1299 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 4004 "parser_gen.cpp"
                    break;

                    case 252:  // double: "-1 (double)"
#line 1302 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 4012 "parser_gen.cpp"
                    break;

                    case 253:  // decimal: "arbitrary decimal"
#line 1308 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 4020 "parser_gen.cpp"
                    break;

                    case 254:  // decimal: "zero (decimal)"
#line 1311 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 4028 "parser_gen.cpp"
                    break;

                    case 255:  // decimal: "1 (decimal)"
#line 1314 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 4036 "parser_gen.cpp"
                    break;

                    case 256:  // decimal: "-1 (decimal)"
#line 1317 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 4044 "parser_gen.cpp"
                    break;

                    case 257:  // bool: "true"
#line 1323 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 4052 "parser_gen.cpp"
                    break;

                    case 258:  // bool: "false"
#line 1326 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 4060 "parser_gen.cpp"
                    break;

                    case 259:  // simpleValue: string
#line 1332 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4066 "parser_gen.cpp"
                    break;

                    case 260:  // simpleValue: aggregationFieldPath
#line 1333 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4072 "parser_gen.cpp"
                    break;

                    case 261:  // simpleValue: variable
#line 1334 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4078 "parser_gen.cpp"
                    break;

                    case 262:  // simpleValue: binary
#line 1335 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4084 "parser_gen.cpp"
                    break;

                    case 263:  // simpleValue: undefined
#line 1336 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4090 "parser_gen.cpp"
                    break;

                    case 264:  // simpleValue: objectId
#line 1337 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4096 "parser_gen.cpp"
                    break;

                    case 265:  // simpleValue: date
#line 1338 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4102 "parser_gen.cpp"
                    break;

                    case 266:  // simpleValue: null
#line 1339 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4108 "parser_gen.cpp"
                    break;

                    case 267:  // simpleValue: regex
#line 1340 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4114 "parser_gen.cpp"
                    break;

                    case 268:  // simpleValue: dbPointer
#line 1341 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4120 "parser_gen.cpp"
                    break;

                    case 269:  // simpleValue: javascript
#line 1342 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4126 "parser_gen.cpp"
                    break;

                    case 270:  // simpleValue: symbol
#line 1343 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4132 "parser_gen.cpp"
                    break;

                    case 271:  // simpleValue: javascriptWScope
#line 1344 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4138 "parser_gen.cpp"
                    break;

                    case 272:  // simpleValue: int
#line 1345 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4144 "parser_gen.cpp"
                    break;

                    case 273:  // simpleValue: long
#line 1346 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4150 "parser_gen.cpp"
                    break;

                    case 274:  // simpleValue: double
#line 1347 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4156 "parser_gen.cpp"
                    break;

                    case 275:  // simpleValue: decimal
#line 1348 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4162 "parser_gen.cpp"
                    break;

                    case 276:  // simpleValue: bool
#line 1349 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4168 "parser_gen.cpp"
                    break;

                    case 277:  // simpleValue: timestamp
#line 1350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4174 "parser_gen.cpp"
                    break;

                    case 278:  // simpleValue: minKey
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4180 "parser_gen.cpp"
                    break;

                    case 279:  // simpleValue: maxKey
#line 1352 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4186 "parser_gen.cpp"
                    break;

                    case 280:  // expressions: %empty
#line 1359 "grammar.yy"
                    {
                    }
#line 4192 "parser_gen.cpp"
                    break;

                    case 281:  // expressions: expressions expression
#line 1360 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 4201 "parser_gen.cpp"
                    break;

                    case 282:  // expression: simpleValue
#line 1367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4207 "parser_gen.cpp"
                    break;

                    case 283:  // expression: expressionObject
#line 1367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4213 "parser_gen.cpp"
                    break;

                    case 284:  // expression: expressionArray
#line 1367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4219 "parser_gen.cpp"
                    break;

                    case 285:  // expression: nonArrayNonObjCompoundExpression
#line 1367 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4225 "parser_gen.cpp"
                    break;

                    case 286:  // nonArrayExpression: simpleValue
#line 1371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4231 "parser_gen.cpp"
                    break;

                    case 287:  // nonArrayExpression: nonArrayCompoundExpression
#line 1371 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4237 "parser_gen.cpp"
                    break;

                    case 288:  // nonArrayCompoundExpression: expressionObject
#line 1375 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4243 "parser_gen.cpp"
                    break;

                    case 289:  // nonArrayCompoundExpression: nonArrayNonObjCompoundExpression
#line 1375 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4249 "parser_gen.cpp"
                    break;

                    case 290:  // nonArrayNonObjCompoundExpression: arrayManipulation
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4255 "parser_gen.cpp"
                    break;

                    case 291:  // nonArrayNonObjCompoundExpression: maths
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4261 "parser_gen.cpp"
                    break;

                    case 292:  // nonArrayNonObjCompoundExpression: meta
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4267 "parser_gen.cpp"
                    break;

                    case 293:  // nonArrayNonObjCompoundExpression: boolExprs
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4273 "parser_gen.cpp"
                    break;

                    case 294:  // nonArrayNonObjCompoundExpression: literalEscapes
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4279 "parser_gen.cpp"
                    break;

                    case 295:  // nonArrayNonObjCompoundExpression: compExprs
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4285 "parser_gen.cpp"
                    break;

                    case 296:  // nonArrayNonObjCompoundExpression: typeExpression
#line 1379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4291 "parser_gen.cpp"
                    break;

                    case 297:  // nonArrayNonObjCompoundExpression: stringExps
#line 1380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4297 "parser_gen.cpp"
                    break;

                    case 298:  // nonArrayNonObjCompoundExpression: setExpression
#line 1380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4303 "parser_gen.cpp"
                    break;

                    case 299:  // nonArrayNonObjCompoundExpression: trig
#line 1380 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4309 "parser_gen.cpp"
                    break;

                    case 300:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1385 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4317 "parser_gen.cpp"
                    break;

                    case 301:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1392 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4325 "parser_gen.cpp"
                    break;

                    case 302:  // compoundNonObjectExpression: expressionArray
#line 1398 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4331 "parser_gen.cpp"
                    break;

                    case 303:  // compoundNonObjectExpression: nonArrayNonObjCompoundExpression
#line 1398 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4337 "parser_gen.cpp"
                    break;

                    case 304:  // arrayManipulation: slice
#line 1402 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4343 "parser_gen.cpp"
                    break;

                    case 305:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1406 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4352 "parser_gen.cpp"
                    break;

                    case 306:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1410 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4361 "parser_gen.cpp"
                    break;

                    case 307:  // expressionArray: "array" expressions "end of array"
#line 1419 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4369 "parser_gen.cpp"
                    break;

                    case 308:  // expressionSingletonArray: "array" expression "end of array"
#line 1426 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 4377 "parser_gen.cpp"
                    break;

                    case 309:  // singleArgExpression: nonArrayExpression
#line 1431 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4383 "parser_gen.cpp"
                    break;

                    case 310:  // singleArgExpression: expressionSingletonArray
#line 1431 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4389 "parser_gen.cpp"
                    break;

                    case 311:  // expressionObject: "object" expressionFields "end of object"
#line 1436 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4397 "parser_gen.cpp"
                    break;

                    case 312:  // expressionFields: %empty
#line 1442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4405 "parser_gen.cpp"
                    break;

                    case 313:  // expressionFields: expressionFields expressionField
#line 1445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4414 "parser_gen.cpp"
                    break;

                    case 314:  // expressionField: expressionFieldname expression
#line 1452 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4422 "parser_gen.cpp"
                    break;

                    case 315:  // expressionFieldname: invariableUserFieldname
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4428 "parser_gen.cpp"
                    break;

                    case 316:  // expressionFieldname: stageAsUserFieldname
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4434 "parser_gen.cpp"
                    break;

                    case 317:  // expressionFieldname: argAsUserFieldname
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4440 "parser_gen.cpp"
                    break;

                    case 318:  // expressionFieldname: idAsUserFieldname
#line 1459 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4446 "parser_gen.cpp"
                    break;

                    case 319:  // idAsUserFieldname: ID
#line 1463 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4454 "parser_gen.cpp"
                    break;

                    case 320:  // idAsProjectionPath: ID
#line 1469 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{makeVector<std::string>("_id")};
                    }
#line 4462 "parser_gen.cpp"
                    break;

                    case 321:  // maths: add
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4468 "parser_gen.cpp"
                    break;

                    case 322:  // maths: abs
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4474 "parser_gen.cpp"
                    break;

                    case 323:  // maths: ceil
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4480 "parser_gen.cpp"
                    break;

                    case 324:  // maths: divide
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4486 "parser_gen.cpp"
                    break;

                    case 325:  // maths: exponent
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4492 "parser_gen.cpp"
                    break;

                    case 326:  // maths: floor
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4498 "parser_gen.cpp"
                    break;

                    case 327:  // maths: ln
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4504 "parser_gen.cpp"
                    break;

                    case 328:  // maths: log
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4510 "parser_gen.cpp"
                    break;

                    case 329:  // maths: logten
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4516 "parser_gen.cpp"
                    break;

                    case 330:  // maths: mod
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4522 "parser_gen.cpp"
                    break;

                    case 331:  // maths: multiply
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4528 "parser_gen.cpp"
                    break;

                    case 332:  // maths: pow
#line 1475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4534 "parser_gen.cpp"
                    break;

                    case 333:  // maths: round
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4540 "parser_gen.cpp"
                    break;

                    case 334:  // maths: sqrt
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4546 "parser_gen.cpp"
                    break;

                    case 335:  // maths: subtract
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4552 "parser_gen.cpp"
                    break;

                    case 336:  // maths: trunc
#line 1476 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4558 "parser_gen.cpp"
                    break;

                    case 337:  // meta: "object" META "geoNearDistance" "end of object"
#line 1480 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4566 "parser_gen.cpp"
                    break;

                    case 338:  // meta: "object" META "geoNearPoint" "end of object"
#line 1483 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4574 "parser_gen.cpp"
                    break;

                    case 339:  // meta: "object" META "indexKey" "end of object"
#line 1486 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4582 "parser_gen.cpp"
                    break;

                    case 340:  // meta: "object" META "randVal" "end of object"
#line 1489 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4590 "parser_gen.cpp"
                    break;

                    case 341:  // meta: "object" META "recordId" "end of object"
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4598 "parser_gen.cpp"
                    break;

                    case 342:  // meta: "object" META "searchHighlights" "end of object"
#line 1495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4606 "parser_gen.cpp"
                    break;

                    case 343:  // meta: "object" META "searchScore" "end of object"
#line 1498 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4614 "parser_gen.cpp"
                    break;

                    case 344:  // meta: "object" META "sortKey" "end of object"
#line 1501 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4622 "parser_gen.cpp"
                    break;

                    case 345:  // meta: "object" META "textScore" "end of object"
#line 1504 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4630 "parser_gen.cpp"
                    break;

                    case 346:  // trig: sin
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4636 "parser_gen.cpp"
                    break;

                    case 347:  // trig: cos
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4642 "parser_gen.cpp"
                    break;

                    case 348:  // trig: tan
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4648 "parser_gen.cpp"
                    break;

                    case 349:  // trig: sinh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4654 "parser_gen.cpp"
                    break;

                    case 350:  // trig: cosh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4660 "parser_gen.cpp"
                    break;

                    case 351:  // trig: tanh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4666 "parser_gen.cpp"
                    break;

                    case 352:  // trig: asin
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4672 "parser_gen.cpp"
                    break;

                    case 353:  // trig: acos
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4678 "parser_gen.cpp"
                    break;

                    case 354:  // trig: atan
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4684 "parser_gen.cpp"
                    break;

                    case 355:  // trig: atan2
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4690 "parser_gen.cpp"
                    break;

                    case 356:  // trig: asinh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4696 "parser_gen.cpp"
                    break;

                    case 357:  // trig: acosh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4702 "parser_gen.cpp"
                    break;

                    case 358:  // trig: atanh
#line 1509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4708 "parser_gen.cpp"
                    break;

                    case 359:  // trig: degreesToRadians
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4714 "parser_gen.cpp"
                    break;

                    case 360:  // trig: radiansToDegrees
#line 1510 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4720 "parser_gen.cpp"
                    break;

                    case 361:  // add: "object" ADD expressionArray "end of object"
#line 1514 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4729 "parser_gen.cpp"
                    break;

                    case 362:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1521 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4738 "parser_gen.cpp"
                    break;

                    case 363:  // abs: "object" ABS expression "end of object"
#line 1527 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4746 "parser_gen.cpp"
                    break;

                    case 364:  // ceil: "object" CEIL expression "end of object"
#line 1532 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4754 "parser_gen.cpp"
                    break;

                    case 365:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4763 "parser_gen.cpp"
                    break;

                    case 366:  // exponent: "object" EXPONENT expression "end of object"
#line 1543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4771 "parser_gen.cpp"
                    break;

                    case 367:  // floor: "object" FLOOR expression "end of object"
#line 1548 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4779 "parser_gen.cpp"
                    break;

                    case 368:  // ln: "object" LN expression "end of object"
#line 1553 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4787 "parser_gen.cpp"
                    break;

                    case 369:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4796 "parser_gen.cpp"
                    break;

                    case 370:  // logten: "object" LOGTEN expression "end of object"
#line 1564 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4804 "parser_gen.cpp"
                    break;

                    case 371:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1569 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4813 "parser_gen.cpp"
                    break;

                    case 372:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1575 "grammar.yy"
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
#line 4825 "parser_gen.cpp"
                    break;

                    case 373:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1584 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4834 "parser_gen.cpp"
                    break;

                    case 374:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1590 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4843 "parser_gen.cpp"
                    break;

                    case 375:  // sqrt: "object" SQRT expression "end of object"
#line 1596 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4851 "parser_gen.cpp"
                    break;

                    case 376:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1601 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4860 "parser_gen.cpp"
                    break;

                    case 377:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1607 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4869 "parser_gen.cpp"
                    break;

                    case 378:  // sin: "object" SIN singleArgExpression "end of object"
#line 1613 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4877 "parser_gen.cpp"
                    break;

                    case 379:  // cos: "object" COS singleArgExpression "end of object"
#line 1618 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4885 "parser_gen.cpp"
                    break;

                    case 380:  // tan: "object" TAN singleArgExpression "end of object"
#line 1623 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4893 "parser_gen.cpp"
                    break;

                    case 381:  // sinh: "object" SINH singleArgExpression "end of object"
#line 1628 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4901 "parser_gen.cpp"
                    break;

                    case 382:  // cosh: "object" COSH singleArgExpression "end of object"
#line 1633 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4909 "parser_gen.cpp"
                    break;

                    case 383:  // tanh: "object" TANH singleArgExpression "end of object"
#line 1638 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::tanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4917 "parser_gen.cpp"
                    break;

                    case 384:  // asin: "object" ASIN singleArgExpression "end of object"
#line 1643 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asin, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4925 "parser_gen.cpp"
                    break;

                    case 385:  // acos: "object" ACOS singleArgExpression "end of object"
#line 1648 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acos, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4933 "parser_gen.cpp"
                    break;

                    case 386:  // atan: "object" ATAN singleArgExpression "end of object"
#line 1653 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4941 "parser_gen.cpp"
                    break;

                    case 387:  // asinh: "object" ASINH singleArgExpression "end of object"
#line 1658 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::asinh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4949 "parser_gen.cpp"
                    break;

                    case 388:  // acosh: "object" ACOSH singleArgExpression "end of object"
#line 1663 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::acosh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4957 "parser_gen.cpp"
                    break;

                    case 389:  // atanh: "object" ATANH singleArgExpression "end of object"
#line 1668 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atanh, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4965 "parser_gen.cpp"
                    break;

                    case 390:  // degreesToRadians: "object" DEGREES_TO_RADIANS singleArgExpression
                               // "end of object"
#line 1673 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4973 "parser_gen.cpp"
                    break;

                    case 391:  // radiansToDegrees: "object" RADIANS_TO_DEGREES singleArgExpression
                               // "end of object"
#line 1678 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees,
                                                         YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4981 "parser_gen.cpp"
                    break;

                    case 392:  // boolExprs: and
#line 1684 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4987 "parser_gen.cpp"
                    break;

                    case 393:  // boolExprs: or
#line 1684 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4993 "parser_gen.cpp"
                    break;

                    case 394:  // boolExprs: not
#line 1684 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4999 "parser_gen.cpp"
                    break;

                    case 395:  // and: "object" AND expressionArray "end of object"
#line 1688 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5008 "parser_gen.cpp"
                    break;

                    case 396:  // or: "object" OR expressionArray "end of object"
#line 1695 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5017 "parser_gen.cpp"
                    break;

                    case 397:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1702 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5026 "parser_gen.cpp"
                    break;

                    case 398:  // stringExps: concat
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5032 "parser_gen.cpp"
                    break;

                    case 399:  // stringExps: dateFromString
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5038 "parser_gen.cpp"
                    break;

                    case 400:  // stringExps: dateToString
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5044 "parser_gen.cpp"
                    break;

                    case 401:  // stringExps: indexOfBytes
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5050 "parser_gen.cpp"
                    break;

                    case 402:  // stringExps: indexOfCP
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5056 "parser_gen.cpp"
                    break;

                    case 403:  // stringExps: ltrim
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5062 "parser_gen.cpp"
                    break;

                    case 404:  // stringExps: regexFind
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5068 "parser_gen.cpp"
                    break;

                    case 405:  // stringExps: regexFindAll
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5074 "parser_gen.cpp"
                    break;

                    case 406:  // stringExps: regexMatch
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5080 "parser_gen.cpp"
                    break;

                    case 407:  // stringExps: replaceOne
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5086 "parser_gen.cpp"
                    break;

                    case 408:  // stringExps: replaceAll
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5092 "parser_gen.cpp"
                    break;

                    case 409:  // stringExps: rtrim
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5098 "parser_gen.cpp"
                    break;

                    case 410:  // stringExps: split
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5104 "parser_gen.cpp"
                    break;

                    case 411:  // stringExps: strLenBytes
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5110 "parser_gen.cpp"
                    break;

                    case 412:  // stringExps: strLenCP
#line 1710 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5116 "parser_gen.cpp"
                    break;

                    case 413:  // stringExps: strcasecmp
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5122 "parser_gen.cpp"
                    break;

                    case 414:  // stringExps: substr
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5128 "parser_gen.cpp"
                    break;

                    case 415:  // stringExps: substrBytes
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5134 "parser_gen.cpp"
                    break;

                    case 416:  // stringExps: substrCP
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5140 "parser_gen.cpp"
                    break;

                    case 417:  // stringExps: toLower
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5146 "parser_gen.cpp"
                    break;

                    case 418:  // stringExps: trim
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5152 "parser_gen.cpp"
                    break;

                    case 419:  // stringExps: toUpper
#line 1711 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5158 "parser_gen.cpp"
                    break;

                    case 420:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1715 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 5170 "parser_gen.cpp"
                    break;

                    case 421:  // formatArg: %empty
#line 1725 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 5178 "parser_gen.cpp"
                    break;

                    case 422:  // formatArg: "format argument" expression
#line 1728 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5186 "parser_gen.cpp"
                    break;

                    case 423:  // timezoneArg: %empty
#line 1734 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 5194 "parser_gen.cpp"
                    break;

                    case 424:  // timezoneArg: "timezone argument" expression
#line 1737 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5202 "parser_gen.cpp"
                    break;

                    case 425:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1744 "grammar.yy"
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
#line 5212 "parser_gen.cpp"
                    break;

                    case 426:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1753 "grammar.yy"
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
#line 5222 "parser_gen.cpp"
                    break;

                    case 427:  // exprZeroToTwo: %empty
#line 1761 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 5230 "parser_gen.cpp"
                    break;

                    case 428:  // exprZeroToTwo: expression
#line 1764 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5238 "parser_gen.cpp"
                    break;

                    case 429:  // exprZeroToTwo: expression expression
#line 1767 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5246 "parser_gen.cpp"
                    break;

                    case 430:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 1774 "grammar.yy"
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
#line 5258 "parser_gen.cpp"
                    break;

                    case 431:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 1785 "grammar.yy"
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
#line 5270 "parser_gen.cpp"
                    break;

                    case 432:  // charsArg: %empty
#line 1795 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 5278 "parser_gen.cpp"
                    break;

                    case 433:  // charsArg: "chars argument" expression
#line 1798 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5286 "parser_gen.cpp"
                    break;

                    case 434:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1804 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5296 "parser_gen.cpp"
                    break;

                    case 435:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1812 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5306 "parser_gen.cpp"
                    break;

                    case 436:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 1820 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 5316 "parser_gen.cpp"
                    break;

                    case 437:  // optionsArg: %empty
#line 1828 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 5324 "parser_gen.cpp"
                    break;

                    case 438:  // optionsArg: "options argument" expression
#line 1831 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5332 "parser_gen.cpp"
                    break;

                    case 439:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 1836 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 5344 "parser_gen.cpp"
                    break;

                    case 440:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 1845 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5352 "parser_gen.cpp"
                    break;

                    case 441:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 1851 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5360 "parser_gen.cpp"
                    break;

                    case 442:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 1857 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5368 "parser_gen.cpp"
                    break;

                    case 443:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1864 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 5379 "parser_gen.cpp"
                    break;

                    case 444:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1874 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 5390 "parser_gen.cpp"
                    break;

                    case 445:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 1883 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5399 "parser_gen.cpp"
                    break;

                    case 446:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 1890 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5408 "parser_gen.cpp"
                    break;

                    case 447:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 1897 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5417 "parser_gen.cpp"
                    break;

                    case 448:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 1905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5426 "parser_gen.cpp"
                    break;

                    case 449:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 1913 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5435 "parser_gen.cpp"
                    break;

                    case 450:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 1921 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5444 "parser_gen.cpp"
                    break;

                    case 451:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 1929 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5453 "parser_gen.cpp"
                    break;

                    case 452:  // toLower: "object" TO_LOWER expression "end of object"
#line 1936 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5461 "parser_gen.cpp"
                    break;

                    case 453:  // toUpper: "object" TO_UPPER expression "end of object"
#line 1942 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5469 "parser_gen.cpp"
                    break;

                    case 454:  // metaSortKeyword: "randVal"
#line 1948 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 5477 "parser_gen.cpp"
                    break;

                    case 455:  // metaSortKeyword: "textScore"
#line 1951 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 5485 "parser_gen.cpp"
                    break;

                    case 456:  // metaSort: "object" META metaSortKeyword "end of object"
#line 1957 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5493 "parser_gen.cpp"
                    break;

                    case 457:  // sortSpecs: "object" specList "end of object"
#line 1963 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5501 "parser_gen.cpp"
                    break;

                    case 458:  // specList: %empty
#line 1968 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5509 "parser_gen.cpp"
                    break;

                    case 459:  // specList: specList sortSpec
#line 1971 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5518 "parser_gen.cpp"
                    break;

                    case 460:  // oneOrNegOne: "1 (int)"
#line 1978 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 5526 "parser_gen.cpp"
                    break;

                    case 461:  // oneOrNegOne: "-1 (int)"
#line 1981 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 5534 "parser_gen.cpp"
                    break;

                    case 462:  // oneOrNegOne: "1 (long)"
#line 1984 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 5542 "parser_gen.cpp"
                    break;

                    case 463:  // oneOrNegOne: "-1 (long)"
#line 1987 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 5550 "parser_gen.cpp"
                    break;

                    case 464:  // oneOrNegOne: "1 (double)"
#line 1990 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 5558 "parser_gen.cpp"
                    break;

                    case 465:  // oneOrNegOne: "-1 (double)"
#line 1993 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 5566 "parser_gen.cpp"
                    break;

                    case 466:  // oneOrNegOne: "1 (decimal)"
#line 1996 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 5574 "parser_gen.cpp"
                    break;

                    case 467:  // oneOrNegOne: "-1 (decimal)"
#line 1999 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 5582 "parser_gen.cpp"
                    break;

                    case 468:  // sortFieldname: valueFieldname
#line 2004 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            SortPath{makeVector<std::string>(stdx::get<UserFieldname>(
                                YY_MOVE(yystack_[0].value.as<CNode::Fieldname>())))};
                    }
#line 5590 "parser_gen.cpp"
                    break;

                    case 469:  // sortFieldname: "fieldname containing dotted path"
#line 2006 "grammar.yy"
                    {
                        auto components = YY_MOVE(yystack_[0].value.as<std::vector<std::string>>());
                        if (auto status = c_node_validation::validateSortPath(components);
                            !status.isOK())
                            error(yystack_[0].location, status.reason());
                        yylhs.value.as<CNode::Fieldname>() = SortPath{std::move(components)};
                    }
#line 5602 "parser_gen.cpp"
                    break;

                    case 470:  // sortSpec: sortFieldname metaSort
#line 2016 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5610 "parser_gen.cpp"
                    break;

                    case 471:  // sortSpec: sortFieldname oneOrNegOne
#line 2018 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5618 "parser_gen.cpp"
                    break;

                    case 472:  // setExpression: allElementsTrue
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5624 "parser_gen.cpp"
                    break;

                    case 473:  // setExpression: anyElementTrue
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5630 "parser_gen.cpp"
                    break;

                    case 474:  // setExpression: setDifference
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5636 "parser_gen.cpp"
                    break;

                    case 475:  // setExpression: setEquals
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5642 "parser_gen.cpp"
                    break;

                    case 476:  // setExpression: setIntersection
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5648 "parser_gen.cpp"
                    break;

                    case 477:  // setExpression: setIsSubset
#line 2024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5654 "parser_gen.cpp"
                    break;

                    case 478:  // setExpression: setUnion
#line 2025 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5660 "parser_gen.cpp"
                    break;

                    case 479:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 2029 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5668 "parser_gen.cpp"
                    break;

                    case 480:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 2035 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5676 "parser_gen.cpp"
                    break;

                    case 481:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 2041 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5685 "parser_gen.cpp"
                    break;

                    case 482:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2049 "grammar.yy"
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
#line 5697 "parser_gen.cpp"
                    break;

                    case 483:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 2060 "grammar.yy"
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
#line 5709 "parser_gen.cpp"
                    break;

                    case 484:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 2070 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5718 "parser_gen.cpp"
                    break;

                    case 485:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 2078 "grammar.yy"
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
#line 5730 "parser_gen.cpp"
                    break;

                    case 486:  // literalEscapes: const
#line 2088 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5736 "parser_gen.cpp"
                    break;

                    case 487:  // literalEscapes: literal
#line 2088 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5742 "parser_gen.cpp"
                    break;

                    case 488:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 2092 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5751 "parser_gen.cpp"
                    break;

                    case 489:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 2099 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5760 "parser_gen.cpp"
                    break;

                    case 490:  // value: simpleValue
#line 2106 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5766 "parser_gen.cpp"
                    break;

                    case 491:  // value: compoundValue
#line 2106 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5772 "parser_gen.cpp"
                    break;

                    case 492:  // compoundValue: valueArray
#line 2110 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5778 "parser_gen.cpp"
                    break;

                    case 493:  // compoundValue: valueObject
#line 2110 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5784 "parser_gen.cpp"
                    break;

                    case 494:  // valueArray: "array" values "end of array"
#line 2114 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5792 "parser_gen.cpp"
                    break;

                    case 495:  // values: %empty
#line 2120 "grammar.yy"
                    {
                    }
#line 5798 "parser_gen.cpp"
                    break;

                    case 496:  // values: values value
#line 2121 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[1].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 5807 "parser_gen.cpp"
                    break;

                    case 497:  // valueObject: "object" valueFields "end of object"
#line 2128 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5815 "parser_gen.cpp"
                    break;

                    case 498:  // valueFields: %empty
#line 2134 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5823 "parser_gen.cpp"
                    break;

                    case 499:  // valueFields: valueFields valueField
#line 2137 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5832 "parser_gen.cpp"
                    break;

                    case 500:  // valueField: valueFieldname value
#line 2144 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5840 "parser_gen.cpp"
                    break;

                    case 501:  // valueFieldname: invariableUserFieldname
#line 2151 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5846 "parser_gen.cpp"
                    break;

                    case 502:  // valueFieldname: stageAsUserFieldname
#line 2152 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5852 "parser_gen.cpp"
                    break;

                    case 503:  // valueFieldname: argAsUserFieldname
#line 2153 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5858 "parser_gen.cpp"
                    break;

                    case 504:  // valueFieldname: aggExprAsUserFieldname
#line 2154 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5864 "parser_gen.cpp"
                    break;

                    case 505:  // valueFieldname: idAsUserFieldname
#line 2155 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5870 "parser_gen.cpp"
                    break;

                    case 506:  // compExprs: cmp
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5876 "parser_gen.cpp"
                    break;

                    case 507:  // compExprs: eq
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5882 "parser_gen.cpp"
                    break;

                    case 508:  // compExprs: gt
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5888 "parser_gen.cpp"
                    break;

                    case 509:  // compExprs: gte
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5894 "parser_gen.cpp"
                    break;

                    case 510:  // compExprs: lt
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5900 "parser_gen.cpp"
                    break;

                    case 511:  // compExprs: lte
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5906 "parser_gen.cpp"
                    break;

                    case 512:  // compExprs: ne
#line 2158 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5912 "parser_gen.cpp"
                    break;

                    case 513:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 2160 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5921 "parser_gen.cpp"
                    break;

                    case 514:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 2165 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5930 "parser_gen.cpp"
                    break;

                    case 515:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 2170 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5939 "parser_gen.cpp"
                    break;

                    case 516:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 2175 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5948 "parser_gen.cpp"
                    break;

                    case 517:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 2180 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5957 "parser_gen.cpp"
                    break;

                    case 518:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 2185 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5966 "parser_gen.cpp"
                    break;

                    case 519:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 2190 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5975 "parser_gen.cpp"
                    break;

                    case 520:  // typeExpression: convert
#line 2196 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5981 "parser_gen.cpp"
                    break;

                    case 521:  // typeExpression: toBool
#line 2197 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5987 "parser_gen.cpp"
                    break;

                    case 522:  // typeExpression: toDate
#line 2198 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5993 "parser_gen.cpp"
                    break;

                    case 523:  // typeExpression: toDecimal
#line 2199 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5999 "parser_gen.cpp"
                    break;

                    case 524:  // typeExpression: toDouble
#line 2200 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6005 "parser_gen.cpp"
                    break;

                    case 525:  // typeExpression: toInt
#line 2201 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6011 "parser_gen.cpp"
                    break;

                    case 526:  // typeExpression: toLong
#line 2202 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6017 "parser_gen.cpp"
                    break;

                    case 527:  // typeExpression: toObjectId
#line 2203 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6023 "parser_gen.cpp"
                    break;

                    case 528:  // typeExpression: toString
#line 2204 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6029 "parser_gen.cpp"
                    break;

                    case 529:  // typeExpression: type
#line 2205 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 6035 "parser_gen.cpp"
                    break;

                    case 530:  // onErrorArg: %empty
#line 2210 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 6043 "parser_gen.cpp"
                    break;

                    case 531:  // onErrorArg: "onError argument" expression
#line 2213 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6051 "parser_gen.cpp"
                    break;

                    case 532:  // onNullArg: %empty
#line 2220 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 6059 "parser_gen.cpp"
                    break;

                    case 533:  // onNullArg: "onNull argument" expression
#line 2223 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 6067 "parser_gen.cpp"
                    break;

                    case 534:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2230 "grammar.yy"
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
#line 6078 "parser_gen.cpp"
                    break;

                    case 535:  // toBool: "object" TO_BOOL expression "end of object"
#line 2239 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6086 "parser_gen.cpp"
                    break;

                    case 536:  // toDate: "object" TO_DATE expression "end of object"
#line 2244 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6094 "parser_gen.cpp"
                    break;

                    case 537:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2249 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6102 "parser_gen.cpp"
                    break;

                    case 538:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 2254 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6110 "parser_gen.cpp"
                    break;

                    case 539:  // toInt: "object" TO_INT expression "end of object"
#line 2259 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6118 "parser_gen.cpp"
                    break;

                    case 540:  // toLong: "object" TO_LONG expression "end of object"
#line 2264 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6126 "parser_gen.cpp"
                    break;

                    case 541:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 2269 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6134 "parser_gen.cpp"
                    break;

                    case 542:  // toString: "object" TO_STRING expression "end of object"
#line 2274 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6142 "parser_gen.cpp"
                    break;

                    case 543:  // type: "object" TYPE expression "end of object"
#line 2279 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 6150 "parser_gen.cpp"
                    break;


#line 6154 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -756;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    -32,  -77,  -70,  -68,  68,   -23,  -756, -756, -756, -756, -756, -756, 51,   21,   63,   918,
    -2,   319,  0,    1,    319,  -756, 67,   -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, 2499,
    -756, -756, -756, -756, -756, -756, -756, -756, 2895, -756, -756, -756, -756, 5,    -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, 86,   -756,
    -756, -756, 70,   -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, 102,  -756, 116,  12,   -23,  -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, 58,   -756, -756, -756, 88,   319,  130,  -756, -756, 1443, 1196, -21,  -70,  -36,  -756,
    3027, -756, -756, 3027, -756, -756, -756, -756, 85,   119,  -756, -756, -756, 2499, -756, -756,
    2499, -25,  3147, -756, -756, -756, -756, 90,   -756, -756, 91,   -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, 1057, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -20,  -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, 1575, 2631,
    2763, 2763, 27,   29,   27,   31,   2763, 2763, 2763, 33,   2763, 2631, 33,   38,   39,   -756,
    2763, 2763, -756, -756, 2763, 40,   33,   2631, 2631, 33,   33,   -756, 41,   42,   52,   2631,
    69,   2631, 33,   33,   -756, 293,  71,   74,   33,   75,   27,   78,   2763, -756, -756, -756,
    -756, -756, 80,   -756, 33,   81,   89,   33,   94,   97,   2763, 2763, 98,   2631, 100,  2631,
    2631, 105,  106,  108,  109,  2763, 2763, 2631, 2631, 2631, 2631, 2631, 2631, 2631, 2631, 2631,
    2631, -756, 110,  2631, 3027, 3027, -756, 158,  114,  11,   3181, -756, 1334, -756, -756, -756,
    -756, -756, 133,  2631, -756, -756, -756, -756, -756, -756, 148,  153,  154,  2631, 178,  2631,
    179,  180,  181,  2631, 184,  185,  187,  188,  -756, 2499, 226,  191,  193,  242,  245,  207,
    2631, 210,  211,  213,  214,  215,  2631, 2631, 2499, 216,  2631, 217,  218,  219,  262,  222,
    225,  230,  233,  235,  236,  238,  254,  255,  2631, 2631, 257,  2631, 270,  2631, 292,  294,
    329,  295,  296,  264,  335,  2631, 262,  300,  2631, 2631, 301,  2631, 2631, 302,  303,  304,
    306,  2631, 307,  2631, 308,  309,  2631, 2631, 2631, 2631, 313,  314,  320,  321,  322,  323,
    324,  325,  326,  330,  331,  336,  262,  2631, 342,  -756, -756, -756, -756, -756, 343,  -756,
    -756, 318,  -756, 345,  -756, -756, -756, 347,  -756, 348,  -756, -756, -756, 2631, -756, -756,
    -756, -756, 1707, 349,  2631, -756, -756, 2631, 2631, -756, 2631, -756, -756, -756, -756, -756,
    2631, 2631, 350,  -756, 2631, -756, -756, -756, 2631, 355,  -756, -756, -756, -756, -756, -756,
    -756, -756, -756, 2631, 2631, -756, 351,  -756, 2631, -756, -756, 2631, -756, -756, 2631, 2631,
    2631, 371,  -756, 2631, 2631, -756, 2631, 2631, -756, -756, -756, -756, 2631, -756, 2631, -756,
    -756, 2631, 2631, 2631, 2631, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, 387,  2631, -756, -756, -756, 2631, -756, -756, -756, -756, -756, -756, 354,  356,  358,
    361,  363,  390,  401,  401,  366,  2631, 2631, 370,  367,  -756, 2631, 374,  -756, 381,  375,
    415,  419,  420,  386,  2631, -756, -756, -756, 1839, 388,  389,  2631, 2631, 2631, 391,  2631,
    392,  -756, -756, -756, -756, -756, -756, 2631, 423,  2631, 414,  414,  393,  2631, 396,  400,
    -756, 402,  403,  404,  1971, -756, 405,  2631, 431,  2631, 2631, 407,  408,  2103, 2235, 2367,
    410,  412,  413,  417,  418,  421,  432,  433,  434,  -756, 2631, 428,  -756, 2631, 390,  423,
    -756, -756, 435,  436,  -756, 437,  -756, 438,  -756, -756, 2631, 422,  444,  -756, 439,  440,
    442,  443,  -756, -756, -756, 445,  446,  447,  -756, 448,  -756, -756, 2631, -756, 423,  449,
    -756, -756, -756, -756, 450,  2631, 2631, -756, -756, -756, -756, -756, -756, -756, -756, 451,
    452,  455,  -756, 456,  457,  458,  459,  -756, 462,  464,  -756, -756, -756, -756};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   75,  3,   458, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   10,  11,  12,  13,  14,  15,  5,   99,  127, 116, 126, 123, 130, 124, 119,
    121, 122, 129, 117, 128, 131, 118, 125, 120, 0,   74,  319, 101, 100, 107, 105, 106, 104, 0,
    114, 76,  78,  79,  0,   156, 209, 212, 132, 195, 134, 196, 208, 211, 210, 133, 213, 157, 139,
    172, 135, 146, 203, 206, 173, 174, 214, 158, 457, 140, 159, 160, 141, 142, 175, 176, 136, 161,
    162, 163, 143, 144, 177, 178, 164, 165, 145, 138, 137, 166, 215, 179, 180, 181, 183, 182, 167,
    184, 197, 198, 199, 200, 201, 168, 202, 205, 185, 169, 108, 111, 112, 113, 110, 109, 188, 186,
    187, 189, 190, 191, 170, 204, 207, 147, 148, 149, 150, 151, 152, 192, 153, 154, 194, 193, 171,
    155, 469, 502, 503, 504, 501, 0,   505, 468, 459, 0,   256, 255, 254, 252, 251, 250, 244, 243,
    242, 248, 247, 246, 241, 245, 249, 253, 19,  20,  21,  22,  24,  26,  0,   23,  0,   0,   6,
    258, 257, 217, 218, 219, 220, 221, 222, 223, 224, 495, 498, 225, 216, 226, 227, 228, 229, 230,
    231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 268, 269, 270, 271, 272, 277, 273, 274, 275,
    278, 279, 95,  259, 260, 262, 263, 264, 276, 265, 266, 267, 490, 491, 492, 493, 261, 82,  80,
    77,  102, 467, 466, 465, 464, 461, 460, 463, 462, 0,   470, 471, 17,  0,   0,   0,   9,   7,
    0,   0,   0,   0,   0,   25,  0,   66,  68,  0,   65,  67,  27,  115, 0,   0,   494, 496, 497,
    0,   499, 81,  0,   0,   0,   83,  84,  85,  86,  103, 454, 455, 0,   59,  58,  55,  54,  57,
    51,  50,  53,  43,  42,  45,  47,  46,  49,  280, 0,   44,  48,  52,  56,  38,  39,  40,  41,
    60,  61,  62,  31,  32,  33,  34,  35,  36,  37,  28,  30,  63,  64,  290, 304, 302, 291, 292,
    321, 293, 392, 393, 394, 294, 486, 487, 297, 398, 399, 400, 401, 402, 403, 404, 405, 406, 407,
    408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 419, 418, 295, 506, 507, 508, 509, 510, 511,
    512, 296, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 322, 323, 324, 325, 326, 327, 328,
    329, 330, 331, 332, 333, 334, 335, 336, 298, 472, 473, 474, 475, 476, 477, 478, 299, 346, 347,
    348, 349, 350, 351, 352, 353, 354, 356, 357, 358, 355, 359, 360, 303, 29,  16,  0,   500, 87,
    82,  96,  89,  92,  94,  93,  91,  98,  456, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   8,   0,   0,   8,   8,   0,   0,   0,   0,   0,   0,   0,
    320, 0,   0,   0,   0,   0,   0,   0,   0,   8,   0,   0,   0,   0,   0,   0,   0,   0,   8,
    8,   8,   8,   8,   0,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   8,   0,   0,
    0,   0,   70,  0,   0,   0,   0,   307, 312, 282, 281, 284, 283, 285, 0,   0,   286, 288, 309,
    287, 289, 310, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   280, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   432, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   432, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    432, 0,   0,   73,  72,  69,  71,  18,  83,  88,  90,  0,   363, 0,   385, 388, 361, 0,   395,
    0,   384, 387, 386, 0,   362, 389, 364, 513, 0,   0,   0,   379, 382, 0,   0,   390, 0,   514,
    366, 367, 515, 516, 0,   0,   0,   368, 0,   370, 517, 518, 0,   0,   337, 338, 339, 340, 341,
    342, 343, 344, 345, 0,   0,   519, 0,   396, 0,   391, 440, 0,   441, 442, 0,   0,   0,   0,
    481, 0,   0,   484, 0,   0,   305, 306, 378, 381, 0,   375, 0,   446, 447, 0,   0,   0,   0,
    380, 383, 535, 536, 537, 538, 539, 540, 452, 541, 542, 453, 0,   0,   543, 97,  311, 0,   316,
    317, 315, 318, 313, 308, 0,   0,   0,   0,   0,   530, 421, 421, 0,   427, 427, 0,   0,   433,
    0,   0,   280, 0,   0,   437, 0,   0,   0,   0,   280, 280, 280, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   314, 479, 480, 300, 420, 488, 0,   532, 0,   423, 423, 0,   428, 0,   0,   489,
    0,   0,   0,   0,   397, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   531, 0,   0,   422, 0,   530, 532, 365, 429, 0,   0,   369, 0,   371,
    0,   373, 438, 0,   0,   0,   374, 0,   0,   0,   0,   301, 445, 448, 0,   0,   0,   376, 0,
    377, 533, 0,   424, 532, 0,   430, 431, 434, 372, 0,   0,   0,   435, 482, 483, 485, 449, 450,
    451, 436, 0,   0,   0,   439, 0,   0,   0,   0,   426, 0,   0,   534, 425, 444, 443};

const short ParserGen::yypgoto_[] = {
    -756, 209,  -756, -756, -186, -14,  -756, -756, -13,  -756, -12,  -756, 229,  -756, -756, -52,
    -756, -756, -244, -238, -232, -230, -228, -10,  -222, -9,   -11,  -4,   -220, -218, -261, -249,
    -756, -216, -214, -212, -756, -208, -206, -240, -44,  -756, -756, -756, -756, -756, -756, -19,
    -756, 328,  -756, -756, -756, -756, -756, -756, -756, -756, -756, 243,  -512, -756, 2,    34,
    -756, 202,  -756, -756, -756, -236, 83,   -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -399, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -326, -755, -251, -289,
    -525, -756, -543, -756, -252, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -756, -257,
    -756, 115,  261,  -756, 87,   -756, -756, -756, -756, -1,   -756, -756, -756, -756, -756, -756,
    -756, -756, -756, -756, -756, -756, -756, -17,  -756};

const short ParserGen::yydefgoto_[] = {
    -1,  532, 276, 758, 154, 155, 277, 156, 157, 158, 159, 533, 160, 56,  278, 534, 763, 286, 57,
    219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237,
    238, 239, 541, 241, 242, 243, 267, 244, 441, 442, 6,   13,  22,  23,  24,  25,  26,  27,  28,
    261, 535, 333, 334, 335, 443, 542, 336, 564, 622, 337, 338, 543, 544, 657, 340, 341, 342, 343,
    344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 607, 360, 361,
    362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 372, 373, 374, 375, 376, 377, 378, 379, 380,
    381, 382, 383, 384, 385, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 396, 397, 398, 399,
    400, 401, 402, 403, 404, 405, 406, 808, 843, 810, 846, 697, 824, 446, 266, 814, 407, 408, 409,
    410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 427, 428,
    429, 430, 550, 551, 545, 553, 554, 8,   14,  268, 247, 269, 58,  59,  291, 292, 293, 294, 60,
    61,  538, 10,  15,  258, 259, 298, 161, 4,   608, 188};

const short ParserGen::yytable_[] = {
    53,  54,  55,  240, 187, 283, 181, 179, 180, 181, 179, 180, 246, 182, 281, 431, 182, 279, 431,
    183, 649, 650, 186, 326, 435, 674, 326, 436, 166, 167, 168, 287, 332, 288, 319, 332, 339, 319,
    5,   339, 320, 440, 321, 320, 322, 321, 7,   322, 9,   438, 323, 296, 324, 323, 325, 324, 327,
    325, 328, 327, 329, 328, 289, 329, 330, 288, 331, 330, 11,  331, 279, 30,  29,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  721, 297, 437, 879, 12,  289,
    609, 610, 47,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    162, 48,  184, 185, 290, 189, 248, 753, 260, 896, 49,  262, 211, 263, 264, 249, 250, 1,   2,
    3,   251, 252, 270, 433, 177, 434, 271, 444, 313, 445, 557, 50,  559, 51,  563, 272, 290, 253,
    254, 568, 569, 576, 582, 583, 255, 256, 16,  17,  18,  19,  20,  21,  653, 584, 31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  586, 658, 600, 192, 193, 601,
    603, 552, 552, 605, 194, 613, 616, 552, 552, 552, 660, 552, 257, 52,  617, 661, 662, 552, 552,
    619, 651, 552, 620, 625, 556, 627, 558, 195, 196, 475, 630, 631, 240, 632, 633, 647, 197, 198,
    273, 274, 664, 666, 667, 668, 199, 552, 670, 671, 820, 672, 673, 240, 676, 677, 240, 678, 829,
    830, 831, 552, 552, 181, 179, 180, 604, 679, 202, 680, 182, 681, 552, 552, 683, 684, 280, 685,
    686, 687, 691, 693, 694, 695, 696, 203, 698, 431, 431, 699, 718, 181, 179, 180, 700, 326, 326,
    701, 182, 702, 703, 440, 704, 279, 332, 332, 319, 319, 339, 339, 273, 274, 320, 320, 321, 321,
    322, 322, 705, 706, 675, 709, 323, 323, 324, 324, 325, 325, 327, 327, 328, 328, 329, 329, 711,
    690, 330, 330, 331, 331, 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  713, 715, 714, 716, 717, 719, 591, 592, 722, 725, 728, 729, 730, 593, 731, 733, 735,
    736, 163, 164, 165, 741, 742, 166, 167, 168, 757, 779, 743, 744, 745, 746, 747, 748, 749, 49,
    594, 595, 750, 751, 169, 170, 171, 788, 752, 596, 597, 172, 173, 174, 755, 756, 764, 598, 765,
    766, 769, 776, 782, 799, 548, 548, 802, 807, 803, 804, 548, 548, 548, 805, 548, 806, 809, 812,
    817, 599, 548, 548, 816, 536, 548, 819, 822, 125, 126, 127, 128, 129, 130, 821, 823, 825, 826,
    827, 845, 833, 834, 842, 838, 840, 885, 848, 570, 850, 548, 573, 574, 851, 858, 876, 852, 853,
    854, 856, 52,  861, 862, 866, 548, 548, 867, 868, 886, 590, 869, 870, 275, 759, 871, 548, 548,
    175, 176, 177, 178, 611, 612, 546, 614, 652, 872, 873, 874, 880, 881, 882, 883, 887, 888, 566,
    889, 890, 285, 891, 892, 893, 894, 897, 898, 901, 902, 578, 579, 903, 904, 905, 906, 907, 646,
    585, 908, 587, 909, 265, 432, 656, 878, 811, 847, 815, 537, 240, 0,   181, 179, 180, 295, 549,
    549, 0,   182, 0,   654, 549, 549, 549, 240, 549, 0,   626, 0,   628, 629, 549, 549, 0,   0,
    549, 0,   636, 637, 638, 639, 640, 641, 642, 643, 644, 645, 0,   555, 648, 0,   0,   0,   560,
    561, 562, 0,   565, 0,   549, 0,   0,   0,   571, 572, 659, 0,   575, 0,   0,   0,   0,   0,
    549, 549, 663, 0,   665, 0,   0,   0,   669, 0,   0,   549, 549, 0,   0,   0,   0,   0,   606,
    0,   0,   682, 0,   0,   0,   0,   0,   688, 689, 0,   0,   692, 623, 624, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   634, 635, 707, 708, 0,   710, 0,   712, 0,   0,   0,   760, 761, 762,
    0,   720, 0,   0,   723, 724, 0,   726, 727, 0,   0,   0,   0,   732, 0,   734, 567, 0,   737,
    738, 739, 740, 0,   0,   0,   0,   577, 0,   0,   580, 581, 0,   0,   0,   0,   754, 0,   0,
    588, 589, 0,   0,   0,   0,   602, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   615,
    767, 0,   618, 0,   621, 0,   0,   770, 0,   0,   771, 772, 0,   773, 0,   0,   0,   0,   0,
    774, 775, 0,   0,   777, 0,   0,   0,   778, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    780, 781, 0,   0,   0,   783, 0,   0,   784, 0,   0,   785, 786, 787, 0,   0,   789, 790, 0,
    791, 792, 0,   0,   0,   0,   793, 0,   794, 0,   0,   795, 796, 797, 798, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   800, 0,   0,   0,   801, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   813, 813, 0,   0,   0,   818, 0,   0,   0,
    0,   0,   0,   0,   0,   828, 0,   0,   0,   832, 0,   0,   835, 836, 837, 0,   839, 0,   0,
    0,   0,   0,   0,   0,   841, 0,   844, 0,   0,   0,   849, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   857, 0,   859, 860, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   875, 0,   0,   877, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   884,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   895, 0,
    0,   0,   0,   0,   0,   0,   0,   899, 900, 62,  63,  64,  65,  66,  67,  68,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  69,  70,  71,  72,  73,  0,
    0,   74,  0,   75,  76,  77,  78,  79,  80,  81,  82,  0,   0,   0,   83,  84,  0,   0,   0,
    0,   85,  86,  0,   87,  88,  0,   0,   89,  90,  49,  91,  92,  0,   0,   0,   0,   93,  94,
    95,  96,  0,   0,   0,   97,  98,  99,  100, 101, 102, 103, 0,   104, 105, 106, 107, 0,   0,
    108, 109, 110, 111, 112, 113, 114, 0,   0,   115, 116, 117, 118, 119, 120, 0,   121, 122, 123,
    124, 125, 126, 127, 128, 129, 130, 0,   0,   131, 132, 133, 134, 135, 136, 137, 138, 139, 0,
    140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 52,  153, 447, 448, 449, 450,
    451, 452, 453, 31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    454, 455, 456, 457, 458, 0,   0,   459, 0,   460, 461, 462, 463, 464, 465, 466, 467, 0,   0,
    0,   468, 469, 0,   0,   0,   0,   0,   470, 0,   471, 472, 0,   0,   473, 474, 475, 476, 477,
    0,   0,   0,   0,   478, 479, 480, 481, 0,   0,   0,   482, 483, 484, 485, 486, 487, 488, 0,
    489, 490, 491, 492, 0,   0,   493, 494, 495, 496, 497, 498, 499, 0,   0,   500, 501, 502, 503,
    504, 505, 0,   506, 507, 508, 509, 0,   0,   0,   0,   0,   0,   0,   0,   510, 511, 512, 513,
    514, 515, 516, 517, 518, 0,   519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531,
    273, 274, 62,  63,  64,  65,  66,  67,  68,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  69,  70,  71,  72,  73,  0,   0,   74,  0,   75,  76,  77,  78,
    79,  80,  81,  82,  0,   0,   0,   83,  84,  0,   0,   0,   0,   284, 86,  0,   87,  88,  0,
    0,   89,  90,  49,  91,  92,  0,   0,   0,   0,   93,  94,  95,  96,  0,   0,   0,   97,  98,
    99,  100, 101, 102, 103, 0,   104, 105, 106, 107, 0,   0,   108, 109, 110, 111, 112, 113, 114,
    0,   0,   115, 116, 117, 118, 119, 120, 0,   121, 122, 123, 124, 125, 126, 127, 128, 129, 130,
    0,   0,   131, 132, 133, 134, 135, 136, 137, 138, 139, 0,   140, 141, 142, 143, 144, 145, 146,
    147, 148, 149, 150, 151, 152, 52,  447, 448, 449, 450, 451, 452, 453, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   454, 455, 456, 457, 458, 0,   0,   459,
    0,   460, 461, 462, 463, 464, 465, 466, 467, 0,   0,   0,   468, 469, 0,   0,   0,   0,   0,
    470, 0,   471, 472, 0,   0,   473, 474, 0,   476, 477, 0,   0,   0,   0,   478, 479, 480, 481,
    0,   0,   0,   482, 483, 484, 485, 486, 487, 488, 0,   489, 490, 491, 492, 0,   0,   493, 494,
    495, 496, 497, 498, 499, 0,   0,   500, 501, 502, 503, 504, 505, 0,   506, 507, 508, 509, 0,
    0,   0,   0,   0,   0,   0,   0,   510, 511, 512, 513, 514, 515, 516, 517, 518, 0,   519, 520,
    521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531, 190, 191, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   163, 164, 165, 0,   0,   166, 167, 168, 282, 0,   0,   0,   0,   0,   192,
    193, 0,   0,   0,   0,   0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,
    197, 198, 0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    200, 201, 0,   0,   0,   0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212,
    213, 214, 215, 175, 176, 177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   163, 164, 165, 0,   0,   166, 167, 168, 539, 0,   0,   0,   0,   0,   192, 193,
    0,   0,   0,   0,   0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197,
    198, 0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313,
    540, 0,   0,   0,   0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213,
    214, 215, 175, 176, 177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   163, 164, 165, 0,   0,   166, 167, 168, 768, 0,   0,   0,   0,   0,   192, 193, 0,
    0,   0,   0,   0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198,
    0,   0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214,
    215, 175, 176, 177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   163, 164, 165, 0,   0,   166, 167, 168, 804, 0,   0,   0,   0,   0,   192, 193, 0,   0,
    0,   0,   0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,
    0,   0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215,
    175, 176, 177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    163, 164, 165, 0,   0,   166, 167, 168, 855, 0,   0,   0,   0,   0,   192, 193, 0,   0,   0,
    0,   0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,
    0,   0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175,
    176, 177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163,
    164, 165, 0,   0,   166, 167, 168, 863, 0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,
    0,   194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,
    0,   0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176,
    177, 178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164,
    165, 0,   0,   166, 167, 168, 864, 0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,
    194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,
    0,   0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177,
    178, 216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164, 165,
    0,   0,   166, 167, 168, 865, 0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194,
    169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,
    0,   199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177, 178,
    216, 217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164, 165, 0,
    0,   166, 167, 168, 0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194, 169,
    170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,
    199, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   200, 201, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177, 178, 216,
    217, 218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164, 165, 0,   0,
    166, 167, 168, 0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194, 169, 170,
    171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,   199,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   313, 540, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177, 178, 216, 217,
    218, 190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164, 165, 0,   0,   166,
    167, 168, 0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194, 169, 170, 171,
    0,   0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    195, 196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,   199, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   547, 540, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   203,
    204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177, 178, 216, 217, 218,
    190, 191, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   163, 164, 165, 0,   0,   166, 167,
    168, 0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194, 169, 170, 171, 0,
    0,   0,   0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   195,
    196, 0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,   199, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   245, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   203, 204,
    205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 175, 176, 177, 178, 216, 217, 218, 299,
    300, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   301, 302, 303, 0,   0,   304, 305, 306,
    0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,   194, 307, 308, 309, 0,   0,
    0,   0,   310, 311, 312, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   195, 196,
    0,   0,   0,   0,   0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,   199, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   313, 314, 0,   0,   0,   0,   0,   0,   0,   0,   0,   202,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   203, 0,   0,
    206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 315, 316, 317, 318, 216, 217, 218, 163, 164,
    165, 0,   0,   166, 167, 168, 0,   0,   0,   0,   0,   0,   192, 193, 0,   0,   0,   0,   0,
    194, 169, 170, 171, 0,   0,   0,   0,   172, 173, 174, 0,   0,   163, 164, 165, 0,   0,   166,
    167, 168, 655, 0,   195, 196, 0,   0,   192, 193, 0,   0,   0,   197, 198, 194, 169, 170, 171,
    0,   0,   199, 0,   172, 173, 174, 0,   0,   0,   0,   0,   0,   439, 0,   0,   0,   0,   0,
    195, 196, 0,   0,   0,   202, 0,   0,   0,   197, 198, 0,   0,   0,   0,   0,   0,   199, 0,
    0,   0,   0,   203, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   175, 176, 177,
    178, 202, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   203,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   175, 176, 177, 178};

const short ParserGen::yycheck_[] = {
    14,  14,  14,  47,  21,  266, 17,  17,  17,  20,  20,  20,  56,  17,  263, 272, 20,  261, 275,
    17,  532, 533, 20,  272, 285, 568, 275, 288, 48,  49,  50,  52,  272, 54,  272, 275, 272, 275,
    115, 275, 272, 290, 272, 275, 272, 275, 116, 275, 116, 289, 272, 87,  272, 275, 272, 275, 272,
    275, 272, 275, 272, 275, 83,  275, 272, 54,  272, 275, 0,   275, 314, 8,   51,  10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  614, 126, 116, 847, 116, 83,
    494, 495, 34,  10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    116, 52,  116, 116, 139, 52,  115, 646, 52,  878, 61,  23,  151, 11,  116, 43,  44,  163, 164,
    165, 48,  49,  78,  52,  158, 20,  52,  51,  115, 52,  115, 82,  115, 84,  115, 61,  139, 65,
    66,  115, 115, 115, 115, 115, 72,  73,  109, 110, 111, 112, 113, 114, 52,  115, 10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  115, 52,  115, 57,  58,  115,
    115, 448, 449, 115, 64,  115, 115, 454, 455, 456, 52,  458, 116, 140, 115, 52,  52,  464, 465,
    115, 52,  468, 115, 115, 450, 115, 452, 87,  88,  61,  115, 115, 266, 115, 115, 115, 96,  97,
    140, 141, 52,  52,  52,  52,  104, 492, 52,  52,  781, 52,  52,  285, 16,  52,  288, 52,  789,
    790, 791, 506, 507, 262, 262, 262, 490, 13,  126, 12,  262, 52,  517, 518, 52,  52,  262, 52,
    52,  52,  52,  52,  52,  52,  10,  143, 52,  532, 533, 52,  14,  290, 290, 290, 52,  532, 533,
    52,  290, 52,  52,  538, 52,  535, 532, 533, 532, 533, 532, 533, 140, 141, 532, 533, 532, 533,
    532, 533, 52,  52,  569, 52,  532, 533, 532, 533, 532, 533, 532, 533, 532, 533, 532, 533, 52,
    584, 532, 533, 532, 533, 10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
    24,  25,  52,  16,  52,  52,  52,  14,  57,  58,  52,  52,  52,  52,  52,  64,  52,  52,  52,
    52,  43,  44,  45,  52,  52,  48,  49,  50,  52,  16,  52,  52,  52,  52,  52,  52,  52,  61,
    87,  88,  52,  52,  65,  66,  67,  16,  52,  96,  97,  72,  73,  74,  52,  52,  51,  104, 51,
    51,  51,  51,  51,  16,  448, 449, 52,  17,  52,  51,  454, 455, 456, 52,  458, 52,  15,  51,
    51,  126, 464, 465, 52,  434, 468, 51,  51,  109, 110, 111, 112, 113, 114, 52,  19,  16,  16,
    51,  24,  51,  51,  18,  51,  51,  22,  52,  463, 51,  492, 466, 467, 51,  21,  25,  52,  52,
    52,  52,  140, 52,  52,  51,  506, 507, 52,  52,  22,  484, 51,  51,  261, 657, 51,  517, 518,
    156, 157, 158, 159, 496, 497, 447, 499, 535, 52,  52,  52,  52,  52,  52,  52,  52,  52,  459,
    52,  52,  267, 52,  52,  52,  52,  52,  52,  52,  52,  471, 472, 52,  52,  52,  52,  52,  529,
    479, 52,  481, 52,  189, 275, 538, 846, 772, 811, 775, 437, 569, -1,  538, 538, 538, 269, 448,
    449, -1,  538, -1,  537, 454, 455, 456, 584, 458, -1,  509, -1,  511, 512, 464, 465, -1,  -1,
    468, -1,  519, 520, 521, 522, 523, 524, 525, 526, 527, 528, -1,  449, 531, -1,  -1,  -1,  454,
    455, 456, -1,  458, -1,  492, -1,  -1,  -1,  464, 465, 547, -1,  468, -1,  -1,  -1,  -1,  -1,
    506, 507, 557, -1,  559, -1,  -1,  -1,  563, -1,  -1,  517, 518, -1,  -1,  -1,  -1,  -1,  492,
    -1,  -1,  576, -1,  -1,  -1,  -1,  -1,  582, 583, -1,  -1,  586, 506, 507, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  517, 518, 600, 601, -1,  603, -1,  605, -1,  -1,  -1,  657, 657, 657,
    -1,  613, -1,  -1,  616, 617, -1,  619, 620, -1,  -1,  -1,  -1,  625, -1,  627, 460, -1,  630,
    631, 632, 633, -1,  -1,  -1,  -1,  470, -1,  -1,  473, 474, -1,  -1,  -1,  -1,  647, -1,  -1,
    482, 483, -1,  -1,  -1,  -1,  488, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  500,
    669, -1,  503, -1,  505, -1,  -1,  676, -1,  -1,  679, 680, -1,  682, -1,  -1,  -1,  -1,  -1,
    688, 689, -1,  -1,  692, -1,  -1,  -1,  696, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    707, 708, -1,  -1,  -1,  712, -1,  -1,  715, -1,  -1,  718, 719, 720, -1,  -1,  723, 724, -1,
    726, 727, -1,  -1,  -1,  -1,  732, -1,  734, -1,  -1,  737, 738, 739, 740, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  754, -1,  -1,  -1,  758, -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  774, 775, -1,  -1,  -1,  779, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  788, -1,  -1,  -1,  792, -1,  -1,  795, 796, 797, -1,  799, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  807, -1,  809, -1,  -1,  -1,  813, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  823, -1,  825, 826, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  842, -1,  -1,  845, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  858,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  876, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  885, 886, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  -1,
    -1,  33,  -1,  35,  36,  37,  38,  39,  40,  41,  42,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,
    -1,  52,  53,  -1,  55,  56,  -1,  -1,  59,  60,  61,  62,  63,  -1,  -1,  -1,  -1,  68,  69,
    70,  71,  -1,  -1,  -1,  75,  76,  77,  78,  79,  80,  81,  -1,  83,  84,  85,  86,  -1,  -1,
    89,  90,  91,  92,  93,  94,  95,  -1,  -1,  98,  99,  100, 101, 102, 103, -1,  105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, -1,  -1,  117, 118, 119, 120, 121, 122, 123, 124, 125, -1,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 3,   4,   5,   6,
    7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  29,  30,  -1,  -1,  33,  -1,  35,  36,  37,  38,  39,  40,  41,  42,  -1,  -1,
    -1,  46,  47,  -1,  -1,  -1,  -1,  -1,  53,  -1,  55,  56,  -1,  -1,  59,  60,  61,  62,  63,
    -1,  -1,  -1,  -1,  68,  69,  70,  71,  -1,  -1,  -1,  75,  76,  77,  78,  79,  80,  81,  -1,
    83,  84,  85,  86,  -1,  -1,  89,  90,  91,  92,  93,  94,  95,  -1,  -1,  98,  99,  100, 101,
    102, 103, -1,  105, 106, 107, 108, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  117, 118, 119, 120,
    121, 122, 123, 124, 125, -1,  127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
    140, 141, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,
    20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  -1,  -1,  33,  -1,  35,  36,  37,  38,
    39,  40,  41,  42,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,  -1,  52,  53,  -1,  55,  56,  -1,
    -1,  59,  60,  61,  62,  63,  -1,  -1,  -1,  -1,  68,  69,  70,  71,  -1,  -1,  -1,  75,  76,
    77,  78,  79,  80,  81,  -1,  83,  84,  85,  86,  -1,  -1,  89,  90,  91,  92,  93,  94,  95,
    -1,  -1,  98,  99,  100, 101, 102, 103, -1,  105, 106, 107, 108, 109, 110, 111, 112, 113, 114,
    -1,  -1,  117, 118, 119, 120, 121, 122, 123, 124, 125, -1,  127, 128, 129, 130, 131, 132, 133,
    134, 135, 136, 137, 138, 139, 140, 3,   4,   5,   6,   7,   8,   9,   -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  26,  27,  28,  29,  30,  -1,  -1,  33,
    -1,  35,  36,  37,  38,  39,  40,  41,  42,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,  -1,  -1,
    53,  -1,  55,  56,  -1,  -1,  59,  60,  -1,  62,  63,  -1,  -1,  -1,  -1,  68,  69,  70,  71,
    -1,  -1,  -1,  75,  76,  77,  78,  79,  80,  81,  -1,  83,  84,  85,  86,  -1,  -1,  89,  90,
    91,  92,  93,  94,  95,  -1,  -1,  98,  99,  100, 101, 102, 103, -1,  105, 106, 107, 108, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  117, 118, 119, 120, 121, 122, 123, 124, 125, -1,  127, 128,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,
    58,  -1,  -1,  -1,  -1,  -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    115, 116, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152,
    153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,
    -1,  -1,  -1,  -1,  -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,
    97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115,
    116, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,
    -1,  -1,  -1,  -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,
    -1,  -1,  -1,  -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154,
    155, 156, 157, 158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  43,  44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,
    -1,  -1,  -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,
    -1,  -1,  -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155,
    156, 157, 158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    43,  44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,
    -1,  -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,
    -1,  -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156,
    157, 158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,
    44,  45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,
    -1,  64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,
    -1,  -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157,
    158, 159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,
    45,  -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,
    64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,
    -1,  -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,
    -1,  -1,  48,  49,  50,  51,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,
    65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,
    -1,  104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,
    -1,  48,  49,  50,  -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,  65,
    66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,
    104, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160,
    161, 162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,
    48,  49,  50,  -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,  65,  66,
    67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161,
    162, 31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,
    49,  50,  -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,  65,  66,  67,
    -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    87,  88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162,
    31,  32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,
    50,  -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,  65,  66,  67,  -1,
    -1,  -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,
    88,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  116, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143, 144,
    145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 31,
    32,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,  50,
    -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,  64,  65,  66,  67,  -1,  -1,
    -1,  -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  115, 116, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  126,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143, -1,  -1,
    146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 43,  44,
    45,  -1,  -1,  48,  49,  50,  -1,  -1,  -1,  -1,  -1,  -1,  57,  58,  -1,  -1,  -1,  -1,  -1,
    64,  65,  66,  67,  -1,  -1,  -1,  -1,  72,  73,  74,  -1,  -1,  43,  44,  45,  -1,  -1,  48,
    49,  50,  51,  -1,  87,  88,  -1,  -1,  57,  58,  -1,  -1,  -1,  96,  97,  64,  65,  66,  67,
    -1,  -1,  104, -1,  72,  73,  74,  -1,  -1,  -1,  -1,  -1,  -1,  115, -1,  -1,  -1,  -1,  -1,
    87,  88,  -1,  -1,  -1,  126, -1,  -1,  -1,  96,  97,  -1,  -1,  -1,  -1,  -1,  -1,  104, -1,
    -1,  -1,  -1,  143, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  156, 157, 158,
    159, 126, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  143,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  156, 157, 158, 159};

const short ParserGen::yystos_[] = {
    0,   163, 164, 165, 364, 115, 214, 116, 344, 116, 358, 0,   116, 215, 345, 359, 109, 110, 111,
    112, 113, 114, 216, 217, 218, 219, 220, 221, 222, 51,  8,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  25,  34,  52,  61,  82,  84,  140, 171, 174, 176, 179,
    184, 349, 350, 355, 356, 3,   4,   5,   6,   7,   8,   9,   26,  27,  28,  29,  30,  33,  35,
    36,  37,  38,  39,  40,  41,  42,  46,  47,  52,  53,  55,  56,  59,  60,  62,  63,  68,  69,
    70,  71,  75,  76,  77,  78,  79,  80,  81,  83,  84,  85,  86,  89,  90,  91,  92,  93,  94,
    95,  98,  99,  100, 101, 102, 103, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 117, 118,
    119, 120, 121, 122, 123, 124, 125, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
    139, 141, 170, 171, 173, 174, 175, 176, 178, 363, 116, 43,  44,  45,  48,  49,  50,  65,  66,
    67,  72,  73,  74,  156, 157, 158, 159, 189, 191, 192, 193, 228, 116, 116, 228, 365, 366, 52,
    31,  32,  57,  58,  64,  87,  88,  96,  97,  104, 115, 116, 126, 143, 144, 145, 146, 147, 148,
    149, 150, 151, 152, 153, 154, 155, 160, 161, 162, 185, 186, 187, 188, 189, 190, 191, 192, 193,
    194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 211, 116, 206,
    347, 115, 43,  44,  48,  49,  65,  66,  72,  73,  116, 360, 361, 52,  223, 23,  11,  116, 215,
    313, 210, 346, 348, 78,  52,  61,  140, 141, 167, 168, 172, 180, 184, 228, 197, 51,  196, 52,
    178, 183, 52,  54,  83,  139, 351, 352, 353, 354, 344, 87,  126, 362, 31,  32,  43,  44,  45,
    48,  49,  50,  65,  66,  67,  72,  73,  74,  115, 116, 156, 157, 158, 159, 185, 186, 187, 188,
    190, 194, 195, 197, 199, 200, 201, 203, 204, 205, 225, 226, 227, 230, 233, 234, 235, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 259,
    260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276, 277, 278,
    279, 280, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297,
    298, 299, 300, 301, 302, 303, 304, 305, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324, 325,
    326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 341, 225, 52,  20,  196, 196,
    116, 205, 115, 197, 212, 213, 228, 51,  52,  312, 3,   4,   5,   6,   7,   8,   9,   26,  27,
    28,  29,  30,  33,  35,  36,  37,  38,  39,  40,  41,  42,  46,  47,  53,  55,  56,  59,  60,
    61,  62,  63,  68,  69,  70,  71,  75,  76,  77,  78,  79,  80,  81,  83,  84,  85,  86,  89,
    90,  91,  92,  93,  94,  95,  98,  99,  100, 101, 102, 103, 105, 106, 107, 108, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
    167, 177, 181, 224, 192, 346, 357, 51,  116, 206, 229, 235, 236, 341, 229, 115, 206, 236, 339,
    340, 341, 342, 343, 343, 235, 115, 235, 115, 343, 343, 343, 115, 231, 343, 229, 231, 115, 115,
    365, 343, 343, 365, 365, 343, 115, 231, 229, 229, 231, 231, 115, 115, 115, 229, 115, 229, 231,
    231, 365, 57,  58,  64,  87,  88,  96,  97,  104, 126, 115, 115, 231, 115, 235, 115, 343, 258,
    365, 258, 258, 365, 365, 115, 365, 231, 115, 115, 231, 115, 115, 231, 232, 343, 343, 115, 229,
    115, 229, 229, 115, 115, 115, 115, 343, 343, 229, 229, 229, 229, 229, 229, 229, 229, 229, 229,
    365, 115, 229, 226, 226, 52,  181, 52,  351, 51,  213, 237, 52,  229, 52,  52,  52,  229, 52,
    229, 52,  52,  52,  229, 52,  52,  52,  52,  312, 196, 16,  52,  52,  13,  12,  52,  229, 52,
    52,  52,  52,  52,  229, 229, 196, 52,  229, 52,  52,  52,  10,  310, 52,  52,  52,  52,  52,
    52,  52,  52,  52,  229, 229, 52,  229, 52,  229, 52,  52,  16,  52,  52,  14,  14,  229, 310,
    52,  229, 229, 52,  229, 229, 52,  52,  52,  52,  229, 52,  229, 52,  52,  229, 229, 229, 229,
    52,  52,  52,  52,  52,  52,  52,  52,  52,  52,  52,  52,  310, 229, 52,  52,  52,  169, 170,
    171, 174, 176, 182, 51,  51,  51,  229, 51,  51,  229, 229, 229, 229, 229, 229, 51,  229, 229,
    16,  229, 229, 51,  229, 229, 229, 229, 229, 16,  229, 229, 229, 229, 229, 229, 229, 229, 229,
    229, 16,  229, 229, 52,  52,  51,  52,  52,  17,  306, 15,  308, 308, 51,  229, 314, 314, 52,
    51,  229, 51,  312, 52,  51,  19,  311, 16,  16,  51,  229, 312, 312, 312, 229, 51,  51,  229,
    229, 229, 51,  229, 51,  229, 18,  307, 229, 24,  309, 309, 52,  229, 51,  51,  52,  52,  52,
    51,  52,  229, 21,  229, 229, 52,  52,  51,  51,  51,  51,  52,  52,  51,  51,  51,  52,  52,
    52,  229, 25,  229, 306, 307, 52,  52,  52,  52,  229, 22,  22,  52,  52,  52,  52,  52,  52,
    52,  52,  229, 307, 52,  52,  229, 229, 52,  52,  52,  52,  52,  52,  52,  52,  52};

const short ParserGen::yyr1_[] = {
    0,   166, 364, 364, 364, 214, 215, 215, 366, 365, 216, 216, 216, 216, 216, 216, 222, 217, 218,
    228, 228, 228, 228, 219, 220, 221, 223, 223, 180, 180, 225, 226, 226, 226, 226, 226, 226, 226,
    226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226, 226,
    226, 226, 226, 226, 226, 226, 226, 226, 167, 168, 168, 168, 227, 224, 224, 181, 181, 344, 345,
    345, 349, 349, 349, 347, 347, 346, 346, 351, 351, 351, 353, 212, 357, 357, 213, 213, 354, 354,
    355, 352, 352, 350, 356, 356, 356, 348, 348, 179, 179, 179, 174, 170, 170, 170, 170, 170, 170,
    171, 172, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 173,
    173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173,
    173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173,
    173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173,
    173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173, 173,
    173, 173, 173, 173, 173, 173, 173, 197, 197, 197, 197, 197, 197, 197, 197, 197, 197, 198, 211,
    199, 200, 201, 203, 204, 205, 185, 186, 187, 188, 190, 194, 195, 189, 189, 189, 189, 191, 191,
    191, 191, 192, 192, 192, 192, 193, 193, 193, 193, 202, 202, 206, 206, 206, 206, 206, 206, 206,
    206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 312, 312, 229, 229, 229,
    229, 339, 339, 340, 340, 341, 341, 341, 341, 341, 341, 341, 341, 341, 341, 231, 232, 230, 230,
    233, 234, 234, 235, 342, 343, 343, 236, 237, 237, 182, 169, 169, 169, 169, 176, 177, 238, 238,
    238, 238, 238, 238, 238, 238, 238, 238, 238, 238, 238, 238, 238, 238, 239, 239, 239, 239, 239,
    239, 239, 239, 239, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323, 323,
    240, 336, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 302, 303, 304, 305, 324, 325,
    326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 337, 338, 241, 241, 241, 242, 243, 244, 248,
    248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248, 248,
    248, 248, 249, 308, 308, 309, 309, 250, 251, 314, 314, 314, 252, 253, 310, 310, 254, 261, 271,
    311, 311, 258, 255, 256, 257, 259, 260, 262, 263, 264, 265, 266, 267, 268, 269, 270, 362, 362,
    360, 358, 359, 359, 361, 361, 361, 361, 361, 361, 361, 361, 175, 175, 363, 363, 315, 315, 315,
    315, 315, 315, 315, 316, 317, 318, 319, 320, 321, 322, 245, 245, 246, 247, 196, 196, 207, 207,
    208, 313, 313, 209, 210, 210, 183, 178, 178, 178, 178, 178, 272, 272, 272, 272, 272, 272, 272,
    273, 274, 275, 276, 277, 278, 279, 280, 280, 280, 280, 280, 280, 280, 280, 280, 280, 306, 306,
    307, 307, 281, 282, 283, 284, 285, 286, 287, 288, 289, 290};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 3, 0, 4, 0,  2,  1, 1, 1,  1, 1, 1, 5, 3, 7, 1,  1,  1, 1, 2, 2, 4, 0, 2, 2,
    2, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 3, 1,  2, 2, 2, 3, 0, 2, 2,  1,  1, 1, 3, 0, 2, 1, 1, 1,
    2, 3, 0, 2, 1, 1, 2, 2, 2,  2,  5, 5, 1,  1, 1, 0, 2, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 0,  2,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  4, 5, 1,  1, 1, 4, 4, 3, 3, 1,  1,  3, 0, 2, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 4, 4,  4,  4, 4, 4, 4, 4, 4, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  4, 4, 4, 4, 7, 4, 4,  4,  7, 4, 7, 8, 7, 7, 4, 7,
    7, 4, 4, 4, 4, 4, 4, 4, 4,  4,  4, 4, 4,  4, 4, 1, 1, 1, 4, 4,  6,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,  1, 6, 0, 2, 0, 2, 11, 10, 0, 1, 2, 8, 8, 0, 2, 8,
    8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4,  7, 8, 8, 8, 4, 4, 1,  1,  4, 3, 0, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 2, 2, 1,  1,  1, 1, 1,  1, 1, 6, 6, 4, 8, 8,  4,  8, 1, 1, 6, 6, 1, 1, 1,
    1, 3, 0, 2, 3, 0, 2, 2, 1,  1,  1, 1, 1,  1, 1, 1, 1, 1, 1, 1,  4,  4, 4, 4, 4, 4, 4, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 0,  2,  0, 2, 11, 4, 4, 4, 4, 4, 4, 4,  4,  4};


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
                                           "COMMENT",
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
                                           "EXISTS",
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
                                           "$@1",
                                           YY_NULLPTR};
#endif


#if YYDEBUG
const short ParserGen::yyrline_[] = {
    0,    354,  354,  357,  360,  367,  373,  374,  382,  382,  385,  385,  385,  385,  385,  385,
    388,  398,  404,  414,  414,  414,  414,  418,  423,  428,  447,  450,  457,  460,  466,  480,
    481,  482,  483,  484,  485,  486,  487,  488,  489,  490,  491,  494,  497,  500,  503,  506,
    509,  512,  515,  518,  521,  524,  527,  530,  533,  536,  539,  542,  545,  546,  547,  548,
    549,  554,  562,  575,  576,  593,  600,  604,  612,  615,  621,  627,  630,  636,  639,  640,
    647,  648,  654,  657,  665,  665,  665,  669,  675,  681,  682,  689,  689,  693,  702,  712,
    718,  723,  733,  741,  742,  743,  746,  749,  756,  756,  756,  759,  767,  770,  773,  776,
    779,  782,  788,  794,  813,  816,  819,  822,  825,  828,  831,  834,  837,  840,  843,  846,
    849,  852,  855,  858,  866,  869,  872,  875,  878,  881,  884,  887,  890,  893,  896,  899,
    902,  905,  908,  911,  914,  917,  920,  923,  926,  929,  932,  935,  938,  941,  944,  947,
    950,  953,  956,  959,  962,  965,  968,  971,  974,  977,  980,  983,  986,  989,  992,  995,
    998,  1001, 1004, 1007, 1010, 1013, 1016, 1019, 1022, 1025, 1028, 1031, 1034, 1037, 1040, 1043,
    1046, 1049, 1052, 1055, 1058, 1061, 1064, 1067, 1070, 1073, 1076, 1079, 1082, 1085, 1088, 1091,
    1094, 1097, 1100, 1103, 1106, 1109, 1112, 1115, 1122, 1127, 1130, 1133, 1136, 1139, 1142, 1145,
    1148, 1151, 1157, 1171, 1185, 1191, 1197, 1203, 1209, 1215, 1221, 1227, 1233, 1239, 1245, 1251,
    1257, 1263, 1266, 1269, 1272, 1278, 1281, 1284, 1287, 1293, 1296, 1299, 1302, 1308, 1311, 1314,
    1317, 1323, 1326, 1332, 1333, 1334, 1335, 1336, 1337, 1338, 1339, 1340, 1341, 1342, 1343, 1344,
    1345, 1346, 1347, 1348, 1349, 1350, 1351, 1352, 1359, 1360, 1367, 1367, 1367, 1367, 1371, 1371,
    1375, 1375, 1379, 1379, 1379, 1379, 1379, 1379, 1379, 1380, 1380, 1380, 1385, 1392, 1398, 1398,
    1402, 1406, 1410, 1419, 1426, 1431, 1431, 1436, 1442, 1445, 1452, 1459, 1459, 1459, 1459, 1463,
    1469, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1475, 1476, 1476, 1476,
    1476, 1480, 1483, 1486, 1489, 1492, 1495, 1498, 1501, 1504, 1509, 1509, 1509, 1509, 1509, 1509,
    1509, 1509, 1509, 1509, 1509, 1509, 1509, 1510, 1510, 1514, 1521, 1527, 1532, 1537, 1543, 1548,
    1553, 1558, 1564, 1569, 1575, 1584, 1590, 1596, 1601, 1607, 1613, 1618, 1623, 1628, 1633, 1638,
    1643, 1648, 1653, 1658, 1663, 1668, 1673, 1678, 1684, 1684, 1684, 1688, 1695, 1702, 1709, 1709,
    1709, 1709, 1709, 1709, 1709, 1710, 1710, 1710, 1710, 1710, 1710, 1710, 1710, 1711, 1711, 1711,
    1711, 1711, 1711, 1711, 1715, 1725, 1728, 1734, 1737, 1743, 1752, 1761, 1764, 1767, 1773, 1784,
    1795, 1798, 1804, 1812, 1820, 1828, 1831, 1836, 1845, 1851, 1857, 1863, 1873, 1883, 1890, 1897,
    1904, 1912, 1920, 1928, 1936, 1942, 1948, 1951, 1957, 1963, 1968, 1971, 1978, 1981, 1984, 1987,
    1990, 1993, 1996, 1999, 2004, 2006, 2016, 2018, 2024, 2024, 2024, 2024, 2024, 2024, 2025, 2029,
    2035, 2041, 2048, 2059, 2070, 2077, 2088, 2088, 2092, 2099, 2106, 2106, 2110, 2110, 2114, 2120,
    2121, 2128, 2134, 2137, 2144, 2151, 2152, 2153, 2154, 2155, 2158, 2158, 2158, 2158, 2158, 2158,
    2158, 2160, 2165, 2170, 2175, 2180, 2185, 2190, 2196, 2197, 2198, 2199, 2200, 2201, 2202, 2203,
    2204, 2205, 2210, 2213, 2220, 2223, 2229, 2239, 2244, 2249, 2254, 2259, 2264, 2269, 2274, 2279};

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
#line 7827 "parser_gen.cpp"

#line 2283 "grammar.yy"
