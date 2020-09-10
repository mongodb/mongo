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
        case symbol_kind::S_atan2:                        // atan2
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
        case symbol_kind::S_match:                        // match
        case symbol_kind::S_predicates:                   // predicates
        case symbol_kind::S_compoundMatchExprs:           // compoundMatchExprs
        case symbol_kind::S_predValue:                    // predValue
        case symbol_kind::S_additionalExprs:              // additionalExprs
        case symbol_kind::S_sortSpecs:                    // sortSpecs
        case symbol_kind::S_specList:                     // specList
        case symbol_kind::S_metaSort:                     // metaSort
        case symbol_kind::S_oneOrNegOne:                  // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:              // metaSortKeyword
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
        case symbol_kind::S_atan2:                        // atan2
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
        case symbol_kind::S_match:                        // match
        case symbol_kind::S_predicates:                   // predicates
        case symbol_kind::S_compoundMatchExprs:           // compoundMatchExprs
        case symbol_kind::S_predValue:                    // predValue
        case symbol_kind::S_additionalExprs:              // additionalExprs
        case symbol_kind::S_sortSpecs:                    // sortSpecs
        case symbol_kind::S_specList:                     // specList
        case symbol_kind::S_metaSort:                     // metaSort
        case symbol_kind::S_oneOrNegOne:                  // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:              // metaSortKeyword
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
        case symbol_kind::S_atan2:                        // atan2
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
        case symbol_kind::S_match:                        // match
        case symbol_kind::S_predicates:                   // predicates
        case symbol_kind::S_compoundMatchExprs:           // compoundMatchExprs
        case symbol_kind::S_predValue:                    // predValue
        case symbol_kind::S_additionalExprs:              // additionalExprs
        case symbol_kind::S_sortSpecs:                    // sortSpecs
        case symbol_kind::S_specList:                     // specList
        case symbol_kind::S_metaSort:                     // metaSort
        case symbol_kind::S_oneOrNegOne:                  // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:              // metaSortKeyword
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
        case symbol_kind::S_atan2:                        // atan2
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
        case symbol_kind::S_match:                        // match
        case symbol_kind::S_predicates:                   // predicates
        case symbol_kind::S_compoundMatchExprs:           // compoundMatchExprs
        case symbol_kind::S_predValue:                    // predValue
        case symbol_kind::S_additionalExprs:              // additionalExprs
        case symbol_kind::S_sortSpecs:                    // sortSpecs
        case symbol_kind::S_specList:                     // specList
        case symbol_kind::S_metaSort:                     // metaSort
        case symbol_kind::S_oneOrNegOne:                  // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:              // metaSortKeyword
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
                case symbol_kind::S_atan2:                        // atan2
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
                case symbol_kind::S_match:                        // match
                case symbol_kind::S_predicates:                   // predicates
                case symbol_kind::S_compoundMatchExprs:           // compoundMatchExprs
                case symbol_kind::S_predValue:                    // predValue
                case symbol_kind::S_additionalExprs:              // additionalExprs
                case symbol_kind::S_sortSpecs:                    // sortSpecs
                case symbol_kind::S_specList:                     // specList
                case symbol_kind::S_metaSort:                     // metaSort
                case symbol_kind::S_oneOrNegOne:                  // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:              // metaSortKeyword
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
#line 331 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1873 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH match
#line 334 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1881 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 4:  // start: START_SORT sortSpecs
#line 337 "src/mongo/db/cst/grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1889 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 5:  // pipeline: "array" stageList "end of array"
#line 344 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1897 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 6:  // stageList: %empty
#line 350 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 1903 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 7:  // stageList: "object" stage "end of object" stageList
#line 351 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1911 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 8:  // $@1: %empty
#line 359 "src/mongo/db/cst/grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1917 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1923 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1929 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1935 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1941 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 14:  // stage: project
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1947 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 15:  // stage: sample
#line 362 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1953 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 16:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 365 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1965 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 17:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 375 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1973 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 18:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 381 "src/mongo/db/cst/grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1986 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 19:  // num: int
#line 391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1992 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 20:  // num: long
#line 391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1998 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 21:  // num: double
#line 391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2004 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 22:  // num: decimal
#line 391 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2010 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 23:  // skip: STAGE_SKIP num
#line 395 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2018 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 24:  // limit: STAGE_LIMIT num
#line 400 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 2026 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 25:  // project: STAGE_PROJECT "object" projectFields "end of object"
#line 405 "src/mongo/db/cst/grammar.yy"
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
#line 2047 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 26:  // projectFields: %empty
#line 424 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2055 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 27:  // projectFields: projectFields projectField
#line 427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2064 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 28:  // projectField: ID topLevelProjection
#line 434 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2072 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 29:  // projectField: aggregationProjectionFieldname topLevelProjection
#line 437 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2080 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 30:  // topLevelProjection: projection
#line 443 "src/mongo/db/cst/grammar.yy"
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
#line 2096 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 31:  // projection: string
#line 457 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 32:  // projection: binary
#line 458 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2108 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 33:  // projection: undefined
#line 459 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 34:  // projection: objectId
#line 460 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 35:  // projection: date
#line 461 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2126 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 36:  // projection: null
#line 462 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2132 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 37:  // projection: regex
#line 463 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 38:  // projection: dbPointer
#line 464 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2144 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 39:  // projection: javascript
#line 465 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2150 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 40:  // projection: symbol
#line 466 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2156 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 41:  // projection: javascriptWScope
#line 467 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 42:  // projection: "1 (int)"
#line 468 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2170 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 43:  // projection: "-1 (int)"
#line 471 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 44:  // projection: "arbitrary integer"
#line 474 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 45:  // projection: "zero (int)"
#line 477 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 46:  // projection: "1 (long)"
#line 480 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 47:  // projection: "-1 (long)"
#line 483 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 48:  // projection: "arbitrary long"
#line 486 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 49:  // projection: "zero (long)"
#line 489 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 50:  // projection: "1 (double)"
#line 492 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 51:  // projection: "-1 (double)"
#line 495 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2242 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 52:  // projection: "arbitrary double"
#line 498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2250 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 53:  // projection: "zero (double)"
#line 501 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2258 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 54:  // projection: "1 (decimal)"
#line 504 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{1.0}}};
                    }
#line 2266 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 55:  // projection: "-1 (decimal)"
#line 507 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{Decimal128{-1.0}}};
                    }
#line 2274 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 56:  // projection: "arbitrary decimal"
#line 510 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 57:  // projection: "zero (decimal)"
#line 513 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2290 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 58:  // projection: "true"
#line 516 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2298 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 59:  // projection: "false"
#line 519 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2306 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 60:  // projection: timestamp
#line 522 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 61:  // projection: minKey
#line 523 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2318 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 62:  // projection: maxKey
#line 524 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2324 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 63:  // projection: projectionObject
#line 525 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 64:  // projection: compoundNonObjectExpression
#line 526 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2336 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 65:  // aggregationProjectionFieldname: projectionFieldname
#line 531 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                        if (stdx::holds_alternative<PositionalProjectionPath>(
                                stdx::get<FieldnamePath>(yylhs.value.as<CNode::Fieldname>())))
                            error(yystack_[0].location,
                                  "positional projection forbidden in $project aggregation "
                                  "pipeline stage");
                    }
#line 2346 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: "fieldname"
#line 539 "src/mongo/db/cst/grammar.yy"
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
#line 2364 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 67:  // projectionFieldname: argAsProjectionPath
#line 552 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2370 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 68:  // projectionFieldname: "fieldname containing dotted path"
#line 553 "src/mongo/db/cst/grammar.yy"
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
#line 2388 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 69:  // projectionObject: "object" projectionObjectFields "end of object"
#line 570 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2396 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 70:  // projectionObjectFields: projectionObjectField
#line 577 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2405 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 71:  // projectionObjectFields: projectionObjectFields
                              // projectionObjectField
#line 581 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2414 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 72:  // projectionObjectField: idAsProjectionPath projection
#line 589 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2422 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 73:  // projectionObjectField: aggregationProjectionFieldname projection
#line 592 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2430 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 74:  // match: "object" predicates "end of object"
#line 598 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2438 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 75:  // predicates: %empty
#line 604 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 76:  // predicates: predicates predicate
#line 607 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2455 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 77:  // predicate: predFieldname predValue
#line 613 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2463 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 78:  // predicate: logicalExpr
#line 616 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2471 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 79:  // predValue: simpleValue
#line 625 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2477 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 80:  // predValue: "object" compoundMatchExprs "end of object"
#line 626 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2485 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 81:  // compoundMatchExprs: %empty
#line 632 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2493 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 82:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 635 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2502 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 83:  // operatorExpression: notExpr
#line 642 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2508 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 84:  // notExpr: NOT regex
#line 645 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2516 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 85:  // notExpr: NOT "object" operatorExpression compoundMatchExprs "end of
                              // object"
#line 649 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[1].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2527 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 86:  // logicalExpr: logicalExprField "array" match additionalExprs "end of
                              // array"
#line 658 "src/mongo/db/cst/grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[1].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2537 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 87:  // logicalExprField: AND
#line 666 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2543 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 88:  // logicalExprField: OR
#line 667 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2549 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 89:  // logicalExprField: NOR
#line 668 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2555 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 90:  // additionalExprs: %empty
#line 671 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2563 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 91:  // additionalExprs: additionalExprs match
#line 674 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2572 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 92:  // predFieldname: idAsUserFieldname
#line 681 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2578 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 93:  // predFieldname: argAsUserFieldname
#line 681 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2584 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 94:  // predFieldname: invariableUserFieldname
#line 681 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2590 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 95:  // invariableUserFieldname: "fieldname"
#line 684 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2598 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 96:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2606 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 97:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 695 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 98:  // stageAsUserFieldname: STAGE_SKIP
#line 698 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2622 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 99:  // stageAsUserFieldname: STAGE_LIMIT
#line 701 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2630 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 100:  // stageAsUserFieldname: STAGE_PROJECT
#line 704 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2638 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 101:  // stageAsUserFieldname: STAGE_SAMPLE
#line 707 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2646 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 102:  // argAsUserFieldname: arg
#line 713 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2654 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 103:  // argAsProjectionPath: arg
#line 719 "src/mongo/db/cst/grammar.yy"
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
#line 2672 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 104:  // arg: "coll argument"
#line 738 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "coll";
                    }
#line 2680 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 105:  // arg: "pipeline argument"
#line 741 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "pipeline";
                    }
#line 2688 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 106:  // arg: "size argument"
#line 744 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "size";
                    }
#line 2696 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 107:  // arg: "input argument"
#line 747 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "input";
                    }
#line 2704 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 108:  // arg: "to argument"
#line 750 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "to";
                    }
#line 2712 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 109:  // arg: "onError argument"
#line 753 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onError";
                    }
#line 2720 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 110:  // arg: "onNull argument"
#line 756 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "onNull";
                    }
#line 2728 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 111:  // arg: "dateString argument"
#line 759 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "dateString";
                    }
#line 2736 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 112:  // arg: "format argument"
#line 762 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "format";
                    }
#line 2744 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 113:  // arg: "timezone argument"
#line 765 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "timezone";
                    }
#line 2752 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 114:  // arg: "date argument"
#line 768 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "date";
                    }
#line 2760 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 115:  // arg: "chars argument"
#line 771 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "chars";
                    }
#line 2768 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 116:  // arg: "regex argument"
#line 774 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "regex";
                    }
#line 2776 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 117:  // arg: "options argument"
#line 777 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "options";
                    }
#line 2784 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 118:  // arg: "find argument"
#line 780 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "find";
                    }
#line 2792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 119:  // arg: "replacement argument"
#line 783 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::string>() = "replacement";
                    }
#line 2800 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 120:  // aggExprAsUserFieldname: ADD
#line 791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2808 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 121:  // aggExprAsUserFieldname: ATAN2
#line 794 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2816 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 122:  // aggExprAsUserFieldname: AND
#line 797 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2824 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 123:  // aggExprAsUserFieldname: CONST_EXPR
#line 800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2832 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 124:  // aggExprAsUserFieldname: LITERAL
#line 803 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2840 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 125:  // aggExprAsUserFieldname: OR
#line 806 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2848 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 126:  // aggExprAsUserFieldname: NOT
#line 809 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2856 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 127:  // aggExprAsUserFieldname: CMP
#line 812 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2864 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 128:  // aggExprAsUserFieldname: EQ
#line 815 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2872 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 129:  // aggExprAsUserFieldname: GT
#line 818 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2880 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 130:  // aggExprAsUserFieldname: GTE
#line 821 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2888 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 131:  // aggExprAsUserFieldname: LT
#line 824 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2896 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 132:  // aggExprAsUserFieldname: LTE
#line 827 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2904 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 133:  // aggExprAsUserFieldname: NE
#line 830 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2912 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 134:  // aggExprAsUserFieldname: CONVERT
#line 833 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2920 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 135:  // aggExprAsUserFieldname: TO_BOOL
#line 836 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2928 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 136:  // aggExprAsUserFieldname: TO_DATE
#line 839 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2936 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 137:  // aggExprAsUserFieldname: TO_DECIMAL
#line 842 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2944 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 138:  // aggExprAsUserFieldname: TO_DOUBLE
#line 845 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2952 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 139:  // aggExprAsUserFieldname: TO_INT
#line 848 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 140:  // aggExprAsUserFieldname: TO_LONG
#line 851 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2968 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 141:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 854 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2976 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: TO_STRING
#line 857 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2984 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: TYPE
#line 860 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2992 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: ABS
#line 863 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 3000 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: CEIL
#line 866 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 3008 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: DIVIDE
#line 869 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 3016 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: EXPONENT
#line 872 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 3024 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: FLOOR
#line 875 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 3032 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: LN
#line 878 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 3040 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: LOG
#line 881 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 3048 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: LOGTEN
#line 884 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 3056 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: MOD
#line 887 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 3064 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: MULTIPLY
#line 890 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 3072 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: POW
#line 893 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 3080 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: ROUND
#line 896 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 3088 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: "slice"
#line 899 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$slice"};
                    }
#line 3096 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: SQRT
#line 902 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 3104 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: SUBTRACT
#line 905 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 3112 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: TRUNC
#line 908 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 3120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: CONCAT
#line 911 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 3128 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 914 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 3136 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 917 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 3144 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 920 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 3152 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 923 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 3160 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: LTRIM
#line 926 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 3168 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: META
#line 929 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 3176 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: REGEX_FIND
#line 932 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3184 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 935 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3192 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: REGEX_MATCH
#line 938 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3200 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: REPLACE_ONE
#line 941 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3208 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: REPLACE_ALL
#line 944 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3216 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: RTRIM
#line 947 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3224 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: SPLIT
#line 950 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3232 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 953 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3240 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: STR_LEN_CP
#line 956 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3248 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 959 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3256 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: SUBSTR
#line 962 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3264 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 965 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3272 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: SUBSTR_CP
#line 968 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3280 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: TO_LOWER
#line 971 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3288 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: TRIM
#line 974 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3296 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: TO_UPPER
#line 977 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3304 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: "allElementsTrue"
#line 980 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 184:  // aggExprAsUserFieldname: "anyElementTrue"
#line 983 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3320 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 185:  // aggExprAsUserFieldname: "setDifference"
#line 986 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3328 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 186:  // aggExprAsUserFieldname: "setEquals"
#line 989 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3336 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 187:  // aggExprAsUserFieldname: "setIntersection"
#line 992 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3344 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 188:  // aggExprAsUserFieldname: "setIsSubset"
#line 995 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3352 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 189:  // aggExprAsUserFieldname: "setUnion"
#line 998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3360 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 190:  // string: "string"
#line 1005 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3368 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 191:  // string: "geoNearDistance"
#line 1010 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearDistance"}};
                    }
#line 3376 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 192:  // string: "geoNearPoint"
#line 1013 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"geoNearPoint"}};
                    }
#line 3384 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 193:  // string: "indexKey"
#line 1016 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"indexKey"}};
                    }
#line 3392 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 194:  // string: "randVal"
#line 1019 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3400 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 195:  // string: "recordId"
#line 1022 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"recordId"}};
                    }
#line 3408 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 196:  // string: "searchHighlights"
#line 1025 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchHighlights"}};
                    }
#line 3416 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 197:  // string: "searchScore"
#line 1028 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"searchScore"}};
                    }
#line 3424 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 198:  // string: "sortKey"
#line 1031 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"sortKey"}};
                    }
#line 3432 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 199:  // string: "textScore"
#line 1034 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3440 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 200:  // aggregationFieldPath: "$-prefixed string"
#line 1040 "src/mongo/db/cst/grammar.yy"
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
#line 3456 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 201:  // variable: "$$-prefixed string"
#line 1054 "src/mongo/db/cst/grammar.yy"
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
#line 3472 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 202:  // binary: "BinData"
#line 1068 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3480 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 203:  // undefined: "undefined"
#line 1074 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3488 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 204:  // objectId: "ObjectID"
#line 1080 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3496 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 205:  // date: "Date"
#line 1086 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3504 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 206:  // null: "null"
#line 1092 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3512 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 207:  // regex: "regex"
#line 1098 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3520 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 208:  // dbPointer: "dbPointer"
#line 1104 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3528 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 209:  // javascript: "Code"
#line 1110 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3536 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 210:  // symbol: "Symbol"
#line 1116 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3544 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 211:  // javascriptWScope: "CodeWScope"
#line 1122 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3552 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 212:  // timestamp: "Timestamp"
#line 1128 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3560 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 213:  // minKey: "minKey"
#line 1134 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3568 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 214:  // maxKey: "maxKey"
#line 1140 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3576 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 215:  // int: "arbitrary integer"
#line 1146 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3584 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 216:  // int: "zero (int)"
#line 1149 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3592 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 217:  // int: "1 (int)"
#line 1152 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3600 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 218:  // int: "-1 (int)"
#line 1155 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3608 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 219:  // long: "arbitrary long"
#line 1161 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3616 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 220:  // long: "zero (long)"
#line 1164 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3624 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 221:  // long: "1 (long)"
#line 1167 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3632 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 222:  // long: "-1 (long)"
#line 1170 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3640 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 223:  // double: "arbitrary double"
#line 1176 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3648 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 224:  // double: "zero (double)"
#line 1179 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3656 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 225:  // double: "1 (double)"
#line 1182 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3664 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 226:  // double: "-1 (double)"
#line 1185 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3672 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 227:  // decimal: "arbitrary decimal"
#line 1191 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3680 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 228:  // decimal: "zero (decimal)"
#line 1194 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3688 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 229:  // decimal: "1 (decimal)"
#line 1197 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3696 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 230:  // decimal: "-1 (decimal)"
#line 1200 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3704 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 231:  // bool: "true"
#line 1206 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3712 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 232:  // bool: "false"
#line 1209 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3720 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 233:  // simpleValue: string
#line 1215 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 234:  // simpleValue: aggregationFieldPath
#line 1216 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3732 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 235:  // simpleValue: variable
#line 1217 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3738 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 236:  // simpleValue: binary
#line 1218 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3744 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 237:  // simpleValue: undefined
#line 1219 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3750 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 238:  // simpleValue: objectId
#line 1220 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3756 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 239:  // simpleValue: date
#line 1221 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3762 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 240:  // simpleValue: null
#line 1222 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3768 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 241:  // simpleValue: regex
#line 1223 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3774 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 242:  // simpleValue: dbPointer
#line 1224 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3780 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 243:  // simpleValue: javascript
#line 1225 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3786 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 244:  // simpleValue: symbol
#line 1226 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 245:  // simpleValue: javascriptWScope
#line 1227 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3798 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 246:  // simpleValue: int
#line 1228 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3804 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 247:  // simpleValue: long
#line 1229 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3810 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 248:  // simpleValue: double
#line 1230 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3816 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 249:  // simpleValue: decimal
#line 1231 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3822 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 250:  // simpleValue: bool
#line 1232 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3828 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 251:  // simpleValue: timestamp
#line 1233 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3834 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 252:  // simpleValue: minKey
#line 1234 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3840 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 253:  // simpleValue: maxKey
#line 1235 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 254:  // expressions: %empty
#line 1242 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 3852 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 255:  // expressions: expression expressions
#line 1243 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3861 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 256:  // expression: simpleValue
#line 1250 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3867 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 257:  // expression: expressionObject
#line 1250 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3873 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 258:  // expression: compoundNonObjectExpression
#line 1250 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3879 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 259:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1255 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3887 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 260:  // exprFixedThreeArg: "array" expression expression expression "end
                               // of array"
#line 1262 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                       YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3895 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 261:  // compoundNonObjectExpression: arrayManipulation
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3901 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 262:  // compoundNonObjectExpression: expressionArray
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3907 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 263:  // compoundNonObjectExpression: maths
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3913 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 264:  // compoundNonObjectExpression: meta
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3919 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 265:  // compoundNonObjectExpression: boolExprs
#line 1268 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3925 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 266:  // compoundNonObjectExpression: literalEscapes
#line 1269 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3931 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 267:  // compoundNonObjectExpression: compExprs
#line 1269 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3937 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 268:  // compoundNonObjectExpression: typeExpression
#line 1269 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3943 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 269:  // compoundNonObjectExpression: stringExps
#line 1269 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3949 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 270:  // compoundNonObjectExpression: setExpression
#line 1269 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3955 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 271:  // arrayManipulation: slice
#line 1273 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3961 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 272:  // slice: "object" "slice" exprFixedTwoArg "end of object"
#line 1277 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3970 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 273:  // slice: "object" "slice" exprFixedThreeArg "end of object"
#line 1281 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::slice, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3979 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 274:  // expressionArray: "array" expressions "end of array"
#line 1290 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3987 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 275:  // expressionObject: "object" expressionFields "end of object"
#line 1298 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3995 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 276:  // expressionFields: %empty
#line 1304 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4003 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 277:  // expressionFields: expressionFields expressionField
#line 1307 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4012 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 278:  // expressionField: expressionFieldname expression
#line 1314 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4020 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 279:  // expressionFieldname: invariableUserFieldname
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4026 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 280:  // expressionFieldname: stageAsUserFieldname
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4032 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 281:  // expressionFieldname: argAsUserFieldname
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4038 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 282:  // expressionFieldname: idAsUserFieldname
#line 1321 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4044 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 283:  // idAsUserFieldname: ID
#line 1325 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 4052 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 284:  // idAsProjectionPath: ID
#line 1331 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            ProjectionPath{make_vector<std::string>("_id")};
                    }
#line 4060 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 285:  // maths: add
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4066 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 286:  // maths: atan2
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4072 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 287:  // maths: abs
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4078 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 288:  // maths: ceil
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4084 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 289:  // maths: divide
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4090 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 290:  // maths: exponent
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4096 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 291:  // maths: floor
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4102 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 292:  // maths: ln
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4108 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 293:  // maths: log
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4114 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 294:  // maths: logten
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4120 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 295:  // maths: mod
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4126 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 296:  // maths: multiply
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4132 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 297:  // maths: pow
#line 1337 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4138 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 298:  // maths: round
#line 1338 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4144 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 299:  // maths: sqrt
#line 1338 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4150 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 300:  // maths: subtract
#line 1338 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4156 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 301:  // maths: trunc
#line 1338 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4162 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 302:  // meta: "object" META "geoNearDistance" "end of object"
#line 1342 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
                    }
#line 4170 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 303:  // meta: "object" META "geoNearPoint" "end of object"
#line 1345 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
                    }
#line 4178 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 304:  // meta: "object" META "indexKey" "end of object"
#line 1348 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
                    }
#line 4186 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 305:  // meta: "object" META "randVal" "end of object"
#line 1351 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
                    }
#line 4194 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 306:  // meta: "object" META "recordId" "end of object"
#line 1354 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
                    }
#line 4202 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 307:  // meta: "object" META "searchHighlights" "end of object"
#line 1357 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
                    }
#line 4210 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 308:  // meta: "object" META "searchScore" "end of object"
#line 1360 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
                    }
#line 4218 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 309:  // meta: "object" META "sortKey" "end of object"
#line 1363 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
                    }
#line 4226 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 310:  // meta: "object" META "textScore" "end of object"
#line 1366 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, CNode{KeyValue::textScore}}}};
                    }
#line 4234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 311:  // add: "object" ADD expressionArray "end of object"
#line 1372 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4243 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 312:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1379 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4252 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 313:  // abs: "object" ABS expression "end of object"
#line 1385 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4260 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 314:  // ceil: "object" CEIL expression "end of object"
#line 1390 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4268 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 315:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1395 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4277 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 316:  // exponent: "object" EXPONENT expression "end of object"
#line 1401 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4285 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 317:  // floor: "object" FLOOR expression "end of object"
#line 1406 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4293 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 318:  // ln: "object" LN expression "end of object"
#line 1411 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4301 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 319:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1416 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4310 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 320:  // logten: "object" LOGTEN expression "end of object"
#line 1422 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4318 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 321:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1427 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4327 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 322:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1433 "src/mongo/db/cst/grammar.yy"
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
#line 4339 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 323:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1442 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4348 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 324:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1448 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4357 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 325:  // sqrt: "object" SQRT expression "end of object"
#line 1454 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4365 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 326:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1459 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4374 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 327:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1465 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4383 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 328:  // boolExprs: and
#line 1471 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4389 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 329:  // boolExprs: or
#line 1471 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4395 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 330:  // boolExprs: not
#line 1471 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4401 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 331:  // and: "object" AND expressionArray "end of object"
#line 1475 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4410 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 332:  // or: "object" OR expressionArray "end of object"
#line 1482 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4419 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 333:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1489 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4428 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 334:  // stringExps: concat
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4434 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 335:  // stringExps: dateFromString
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4440 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 336:  // stringExps: dateToString
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4446 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 337:  // stringExps: indexOfBytes
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4452 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 338:  // stringExps: indexOfCP
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4458 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 339:  // stringExps: ltrim
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4464 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 340:  // stringExps: regexFind
#line 1496 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4470 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 341:  // stringExps: regexFindAll
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4476 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 342:  // stringExps: regexMatch
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4482 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 343:  // stringExps: replaceOne
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4488 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 344:  // stringExps: replaceAll
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4494 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 345:  // stringExps: rtrim
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4500 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 346:  // stringExps: split
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4506 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 347:  // stringExps: strLenBytes
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4512 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 348:  // stringExps: strLenCP
#line 1497 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4518 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 349:  // stringExps: strcasecmp
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 350:  // stringExps: substr
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4530 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 351:  // stringExps: substrBytes
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4536 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 352:  // stringExps: substrCP
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4542 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 353:  // stringExps: toLower
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4548 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 354:  // stringExps: trim
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4554 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 355:  // stringExps: toUpper
#line 1498 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4560 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 356:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1502 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 4572 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 357:  // formatArg: %empty
#line 1512 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 4580 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 358:  // formatArg: "format argument" expression
#line 1515 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4588 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 359:  // timezoneArg: %empty
#line 1521 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 4596 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 360:  // timezoneArg: "timezone argument" expression
#line 1524 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4604 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 361:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1531 "src/mongo/db/cst/grammar.yy"
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
#line 4614 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 362:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1540 "src/mongo/db/cst/grammar.yy"
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
#line 4624 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 363:  // exprZeroToTwo: %empty
#line 1548 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4632 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 364:  // exprZeroToTwo: expression
#line 1551 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4640 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 365:  // exprZeroToTwo: expression expression
#line 1554 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4648 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 366:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 1561 "src/mongo/db/cst/grammar.yy"
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
#line 4660 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 367:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 1572 "src/mongo/db/cst/grammar.yy"
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
#line 4672 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 368:  // charsArg: %empty
#line 1582 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4680 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 369:  // charsArg: "chars argument" expression
#line 1585 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4688 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 370:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1591 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4698 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 371:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1599 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4708 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 372:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 1607 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4718 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 373:  // optionsArg: %empty
#line 1615 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4726 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 374:  // optionsArg: "options argument" expression
#line 1618 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4734 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 375:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 1623 "src/mongo/db/cst/grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4746 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 376:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 1632 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4754 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 377:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 1638 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4762 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 378:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 1644 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4770 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 379:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1651 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4781 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 380:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1661 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4792 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 381:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 1670 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4801 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 382:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 1677 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4810 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 383:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 1684 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4819 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 384:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 1692 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4828 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 385:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 1700 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4837 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 386:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 1708 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4846 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 387:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 1716 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4855 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 388:  // toLower: "object" TO_LOWER expression "end of object"
#line 1723 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4863 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 389:  // toUpper: "object" TO_UPPER expression "end of object"
#line 1729 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4871 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 390:  // metaSortKeyword: "randVal"
#line 1735 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4879 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 391:  // metaSortKeyword: "textScore"
#line 1738 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4887 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 392:  // metaSort: "object" META metaSortKeyword "end of object"
#line 1744 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4895 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 393:  // sortSpecs: "object" specList "end of object"
#line 1750 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4903 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 394:  // specList: %empty
#line 1755 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4911 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 395:  // specList: specList sortSpec
#line 1758 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4920 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 396:  // oneOrNegOne: "1 (int)"
#line 1765 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4928 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 397:  // oneOrNegOne: "-1 (int)"
#line 1768 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4936 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 398:  // oneOrNegOne: "1 (long)"
#line 1771 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4944 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 399:  // oneOrNegOne: "-1 (long)"
#line 1774 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4952 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 400:  // oneOrNegOne: "1 (double)"
#line 1777 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4960 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 401:  // oneOrNegOne: "-1 (double)"
#line 1780 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 4968 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 402:  // oneOrNegOne: "1 (decimal)"
#line 1783 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 4976 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 403:  // oneOrNegOne: "-1 (decimal)"
#line 1786 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 4984 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 404:  // sortSpec: valueFieldname metaSort
#line 1791 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4992 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 405:  // sortSpec: valueFieldname oneOrNegOne
#line 1793 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5000 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 406:  // setExpression: allElementsTrue
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5006 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 407:  // setExpression: anyElementTrue
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5012 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 408:  // setExpression: setDifference
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5018 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 409:  // setExpression: setEquals
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5024 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 410:  // setExpression: setIntersection
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5030 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 411:  // setExpression: setIsSubset
#line 1799 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5036 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 412:  // setExpression: setUnion
#line 1800 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5042 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 413:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 1804 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5050 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 414:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 1810 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 5058 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 415:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 1816 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5067 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 416:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 1824 "src/mongo/db/cst/grammar.yy"
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
#line 5079 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 417:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 1835 "src/mongo/db/cst/grammar.yy"
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
#line 5091 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 418:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 1845 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5100 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 419:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 1853 "src/mongo/db/cst/grammar.yy"
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
#line 5112 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 420:  // literalEscapes: const
#line 1863 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5118 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 421:  // literalEscapes: literal
#line 1863 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5124 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 422:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 1867 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5133 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 423:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 1874 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 5142 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 424:  // value: simpleValue
#line 1881 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5148 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 425:  // value: compoundValue
#line 1881 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5154 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 426:  // compoundValue: valueArray
#line 1885 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5160 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 427:  // compoundValue: valueObject
#line 1885 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5166 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 428:  // valueArray: "array" values "end of array"
#line 1889 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 5174 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 429:  // values: %empty
#line 1895 "src/mongo/db/cst/grammar.yy"
                    {
                    }
#line 5180 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 430:  // values: value values
#line 1896 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 5189 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 431:  // valueObject: "object" valueFields "end of object"
#line 1903 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 5197 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 432:  // valueFields: %empty
#line 1909 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 5205 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 433:  // valueFields: valueFields valueField
#line 1912 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 5214 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 434:  // valueField: valueFieldname value
#line 1919 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5222 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 435:  // valueFieldname: invariableUserFieldname
#line 1926 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5228 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 436:  // valueFieldname: stageAsUserFieldname
#line 1927 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5234 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 437:  // valueFieldname: argAsUserFieldname
#line 1928 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5240 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 438:  // valueFieldname: aggExprAsUserFieldname
#line 1929 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5246 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 439:  // valueFieldname: idAsUserFieldname
#line 1930 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 5252 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 440:  // compExprs: cmp
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5258 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 441:  // compExprs: eq
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5264 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 442:  // compExprs: gt
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5270 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 443:  // compExprs: gte
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5276 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 444:  // compExprs: lt
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5282 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 445:  // compExprs: lte
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5288 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 446:  // compExprs: ne
#line 1933 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5294 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 447:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 1935 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5303 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 448:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 1940 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5312 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 449:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 1945 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5321 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 450:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 1950 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5330 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 451:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 1955 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5339 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 452:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 1960 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5348 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 453:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 1965 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5357 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 454:  // typeExpression: convert
#line 1971 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5363 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 455:  // typeExpression: toBool
#line 1972 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5369 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 456:  // typeExpression: toDate
#line 1973 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5375 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 457:  // typeExpression: toDecimal
#line 1974 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5381 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 458:  // typeExpression: toDouble
#line 1975 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5387 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 459:  // typeExpression: toInt
#line 1976 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5393 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 460:  // typeExpression: toLong
#line 1977 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5399 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 461:  // typeExpression: toObjectId
#line 1978 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5405 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 462:  // typeExpression: toString
#line 1979 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5411 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 463:  // typeExpression: type
#line 1980 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5417 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 464:  // onErrorArg: %empty
#line 1985 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 5425 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 465:  // onErrorArg: "onError argument" expression
#line 1988 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5433 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 466:  // onNullArg: %empty
#line 1995 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 5441 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 467:  // onNullArg: "onNull argument" expression
#line 1998 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5449 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 468:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 2005 "src/mongo/db/cst/grammar.yy"
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
#line 5460 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 469:  // toBool: "object" TO_BOOL expression "end of object"
#line 2014 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5468 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 470:  // toDate: "object" TO_DATE expression "end of object"
#line 2019 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5476 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 471:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 2024 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5484 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 472:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 2029 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5492 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 473:  // toInt: "object" TO_INT expression "end of object"
#line 2034 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5500 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 474:  // toLong: "object" TO_LONG expression "end of object"
#line 2039 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5508 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 475:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 2044 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5516 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 476:  // toString: "object" TO_STRING expression "end of object"
#line 2049 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5524 "src/mongo/db/cst/parser_gen.cpp"
                    break;

                    case 477:  // type: "object" TYPE expression "end of object"
#line 2054 "src/mongo/db/cst/grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5532 "src/mongo/db/cst/parser_gen.cpp"
                    break;


#line 5536 "src/mongo/db/cst/parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -690;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    7,    -93,  -85,  -76,  37,   -6,   -690, -690, -690, -690, -690, -690, 82,   61,   1582, 610,
    6,    306,  9,    12,   306,  -690, 77,   -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, 1198, -690, -690, -690, 19,   -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, 293,  -690,
    80,   -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, 106,  -690, 124,  34,   -6,   -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -85,
    -690, -690, -690, -690, -690, -690, -690, -690, 73,   -690, -690, -690, 1598, 306,  84,   -690,
    -690, 49,   -690, -60,  -690, 1320, -690, -690, 1320, -690, -690, -690, -690, 101,  126,  -690,
    -20,  -690, -690, -14,  -690, -690, 104,  -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, 954,  461,  -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, 46,   76,
    -690, -690, -690, -690, 1464, -690, 954,  -690, -690, 108,  954,  60,   62,   60,   65,   66,
    954,  66,   69,   70,   -690, -690, -690, 71,   66,   954,  954,  66,   66,   -690, 72,   74,
    83,   954,  88,   954,  66,   66,   -690, 111,  89,   90,   66,   91,   60,   92,   -690, -690,
    -690, -690, -690, 100,  -690, 66,   103,  107,  66,   112,  114,  115,  954,  116,  954,  954,
    117,  118,  119,  121,  954,  954,  954,  954,  954,  954,  954,  954,  954,  954,  -690, 122,
    954,  1320, 1320, -690, 1648, 120,  -690, 1665, -690, -690, 141,  160,  954,  163,  954,  954,
    167,  170,  182,  954,  1076, 217,  235,  238,  954,  208,  224,  226,  233,  234,  954,  954,
    1076, 236,  954,  239,  240,  243,  269,  244,  245,  247,  248,  249,  250,  253,  254,  260,
    954,  954,  265,  954,  267,  954,  268,  302,  275,  279,  311,  312,  954,  269,  283,  954,
    954,  287,  954,  954,  288,  291,  954,  292,  954,  295,  301,  954,  954,  954,  954,  307,
    308,  309,  310,  315,  316,  321,  322,  323,  324,  269,  954,  328,  -690, -690, -690, -690,
    -690, 53,   -690, 954,  -690, -690, -690, -690, -690, -690, -690, 294,  -690, 330,  954,  -690,
    -690, -690, 332,  1076, -690, 334,  -690, -690, -690, -690, 954,  954,  954,  954,  -690, -690,
    -690, -690, -690, 954,  954,  335,  -690, 954,  -690, -690, -690, 954,  339,  -690, -690, -690,
    -690, -690, -690, -690, -690, -690, 954,  954,  -690, 336,  -690, 954,  -690, 954,  -690, -690,
    954,  954,  954,  364,  -690, 954,  954,  -690, 954,  954,  -690, -690, 954,  -690, 954,  -690,
    -690, 954,  954,  954,  954,  -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, 365,
    954,  -690, -690, -690, 338,  340,  342,  343,  1076, 345,  732,  349,  366,  371,  371,  351,
    954,  954,  352,  355,  -690, 954,  356,  954,  359,  357,  376,  388,  389,  363,  954,  954,
    954,  954,  832,  367,  368,  954,  954,  954,  369,  954,  370,  -690, -690, -690, -690, -690,
    -690, -690, 1076, -690, -690, 954,  390,  954,  385,  385,  372,  954,  374,  377,  -690, 378,
    379,  380,  382,  -690, 383,  954,  393,  954,  954,  384,  386,  391,  392,  394,  395,  396,
    397,  399,  401,  402,  408,  409,  410,  -690, -690, 954,  404,  -690, 954,  366,  390,  -690,
    -690, 411,  412,  -690, 413,  -690, 414,  -690, -690, 954,  417,  424,  -690, 415,  419,  420,
    421,  -690, -690, -690, 444,  445,  453,  -690, 454,  -690, -690, 954,  -690, 390,  455,  -690,
    -690, -690, -690, 457,  954,  954,  -690, -690, -690, -690, -690, -690, -690, -690, 458,  459,
    460,  -690, 465,  466,  472,  473,  -690, 474,  475,  -690, -690, -690, -690};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   75,  3,   394, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   10,  11,  12,  13,  14,  15,  5,   87,  115, 104, 114, 111, 118, 112, 107,
    109, 110, 117, 105, 116, 119, 106, 113, 108, 74,  283, 89,  88,  95,  93,  94,  92,  0,   102,
    76,  78,  0,   144, 120, 183, 122, 184, 121, 145, 127, 160, 123, 134, 161, 162, 146, 393, 128,
    147, 148, 129, 130, 163, 164, 124, 149, 150, 151, 131, 132, 165, 166, 152, 153, 133, 126, 125,
    154, 167, 168, 169, 171, 170, 155, 172, 185, 186, 187, 188, 189, 156, 173, 157, 96,  99,  100,
    101, 98,  97,  176, 174, 175, 177, 178, 179, 158, 135, 136, 137, 138, 139, 140, 180, 141, 142,
    182, 181, 159, 143, 436, 437, 438, 435, 439, 0,   395, 0,   230, 229, 228, 226, 225, 224, 218,
    217, 216, 222, 221, 220, 215, 219, 223, 227, 19,  20,  21,  22,  24,  26,  0,   23,  0,   0,
    6,   232, 231, 191, 192, 193, 194, 195, 196, 197, 198, 81,  199, 190, 200, 201, 202, 203, 204,
    205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 242, 243, 244, 245, 246, 251, 247, 248, 249,
    252, 253, 233, 234, 236, 237, 238, 250, 239, 240, 241, 79,  235, 77,  0,   403, 402, 401, 400,
    397, 396, 399, 398, 0,   404, 405, 17,  0,   0,   0,   9,   7,   0,   90,  0,   25,  0,   66,
    68,  0,   65,  67,  27,  103, 0,   0,   80,  0,   82,  83,  0,   390, 391, 0,   59,  58,  55,
    54,  57,  51,  50,  53,  43,  42,  45,  47,  46,  49,  254, 0,   44,  48,  52,  56,  38,  39,
    40,  41,  60,  61,  62,  31,  32,  33,  34,  35,  36,  37,  28,  30,  63,  64,  261, 271, 262,
    263, 264, 285, 286, 265, 328, 329, 330, 266, 420, 421, 269, 334, 335, 336, 337, 338, 339, 340,
    341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 355, 354, 267, 440, 441, 442,
    443, 444, 445, 446, 268, 454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 287, 288, 289, 290,
    291, 292, 293, 294, 295, 296, 297, 298, 299, 300, 301, 270, 406, 407, 408, 409, 410, 411, 412,
    29,  16,  0,   0,   84,  86,  91,  392, 276, 256, 254, 258, 257, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   8,   8,   8,   0,   0,   0,   0,   0,   0,   284, 0,   0,   0,   0,
    0,   0,   0,   0,   8,   0,   0,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   0,   8,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   8,   0,   0,   0,   0,   70,  0,   0,   81,  0,   255, 274, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   254, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   368, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   368, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   368, 0,   0,   73,  72,  69,  71,  18,  0,   275, 0,   280, 281, 279, 282, 277, 313,
    311, 0,   331, 0,   0,   312, 314, 447, 0,   429, 432, 0,   424, 425, 426, 427, 0,   0,   0,
    0,   448, 316, 317, 449, 450, 0,   0,   0,   318, 0,   320, 451, 452, 0,   0,   302, 303, 304,
    305, 306, 307, 308, 309, 310, 0,   0,   453, 0,   332, 0,   376, 0,   377, 378, 0,   0,   0,
    0,   415, 0,   0,   418, 0,   0,   272, 273, 0,   325, 0,   382, 383, 0,   0,   0,   0,   469,
    470, 471, 472, 473, 474, 388, 475, 476, 389, 0,   0,   477, 85,  278, 0,   0,   0,   0,   429,
    0,   0,   0,   464, 357, 357, 0,   363, 363, 0,   0,   369, 0,   0,   254, 0,   0,   373, 0,
    0,   0,   0,   254, 254, 254, 0,   0,   0,   0,   0,   0,   0,   0,   0,   413, 414, 259, 356,
    430, 428, 431, 0,   433, 422, 0,   466, 0,   359, 359, 0,   364, 0,   0,   423, 0,   0,   0,
    0,   333, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   434, 465, 0,   0,   358, 0,   464, 466, 315, 365, 0,   0,   319, 0,   321, 0,   323,
    374, 0,   0,   0,   324, 0,   0,   0,   0,   260, 381, 384, 0,   0,   0,   326, 0,   327, 467,
    0,   360, 466, 0,   366, 367, 370, 322, 0,   0,   0,   371, 416, 417, 419, 385, 386, 387, 372,
    0,   0,   0,   375, 0,   0,   0,   0,   362, 0,   0,   468, 361, 380, 379};

const short ParserGen::yypgoto_[] = {
    -690, 177,  -690, -690, -47,  -13,  -690, -690, -12,  -11,  -690, -236, -690, -690, -37,  -690,
    -690, -230, -232, -207, -205, -203, 2,    -201, 14,   -8,   63,   -196, -194, -461, -209, -690,
    -192, -186, -172, -690, -167, -151, -238, -55,  -690, -690, -690, -690, -690, -690, 263,  -690,
    -690, -690, -690, -690, -690, -690, -690, -690, 255,  -433, -690, 5,    -389, -147, -308, -690,
    -690, -690, -318, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -315, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -690,
    -690, -690, -690, -690, -690, -690, -690, -690, -690, -690, -227, -689, -138, -190, -479, -690,
    -376, -128, -136, -690, -690, -690, -690, -690, -690, -690, -690, -199, -690, 75,   -690, -690,
    -690, -690, 173,  -690, -690, -690, -690, -690, -690, -690, -690, -690, -17,  -690};

const short ParserGen::yydefgoto_[] = {
    -1,  465, 249, 563, 137, 138, 250, 139, 140, 141, 466, 142, 55,  251, 467, 568, 707, 56,
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 664, 211, 212, 213, 214, 215, 216,
    217, 218, 219, 389, 583, 584, 585, 666, 221, 6,   13,  22,  23,  24,  25,  26,  27,  28,
    236, 468, 297, 298, 299, 165, 390, 391, 480, 533, 301, 302, 303, 392, 471, 304, 305, 306,
    307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319, 320, 321, 322, 323, 324,
    518, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339, 340, 341,
    342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359,
    360, 361, 362, 363, 364, 365, 366, 367, 368, 369, 370, 371, 710, 746, 712, 749, 604, 726,
    393, 665, 716, 372, 373, 374, 375, 376, 377, 378, 379, 8,   14,  241, 222, 259, 57,  58,
    257, 258, 59,  10,  15,  233, 234, 262, 143, 4,   519, 170};

const short ParserGen::yytable_[] = {
    220, 52,  53,  54,  169, 474, 252, 296, 5,   163, 296, 481, 163, 283, 472, 260, 283, 7,   384,
    161, 490, 491, 161, 581, 242, 168, 9,   385, 497, 254, 499, 162, 556, 557, 162, 597, 290, 11,
    284, 290, 285, 284, 286, 285, 287, 286, 627, 287, 252, 288, 261, 289, 288, 291, 289, 535, 291,
    537, 538, 292, 386, 782, 292, 543, 544, 545, 546, 547, 548, 549, 550, 551, 552, 293, 655, 555,
    293, 475, 294, 477, 164, 294, 383, 164, 148, 149, 150, 571, 7,   573, 574, 255, 799, 482, 295,
    658, 12,  295, 300, 589, 489, 300, 29,  492, 493, 595, 596, 578, 144, 599, 516, 166, 500, 501,
    167, 192, 520, 521, 514, 171, 223, 256, 235, 614, 615, 256, 617, 237, 619, 526, 174, 175, 529,
    238, 532, 626, 239, 176, 629, 630, 243, 632, 633, 381, 382, 636, 387, 638, 256, 473, 641, 642,
    643, 644, 1,   2,   3,   503, 504, 177, 178, 277, 560, 476, 505, 656, 478, 479, 179, 180, 483,
    484, 488, 494, 659, 495, 181, 16,  17,  18,  19,  20,  21,  569, 496, 662, 506, 507, 159, 498,
    512, 513, 515, 517, 183, 508, 509, 668, 669, 670, 671, 524, 570, 510, 527, 572, 672, 673, 528,
    575, 675, 184, 576, 530, 676, 531, 534, 536, 539, 540, 541, 511, 542, 554, 577, 678, 679, 296,
    296, 163, 681, 586, 682, 283, 283, 683, 684, 685, 252, 161, 687, 688, 253, 689, 690, 743, 587,
    691, 588, 692, 590, 162, 693, 694, 695, 696, 290, 290, 284, 284, 285, 285, 286, 286, 287, 287,
    591, 698, 592, 288, 288, 289, 289, 291, 291, 593, 594, 603, 598, 292, 292, 600, 601, 715, 715,
    602, 605, 606, 720, 607, 608, 609, 610, 293, 293, 611, 612, 730, 294, 294, 164, 734, 613, 722,
    737, 738, 739, 616, 741, 618, 620, 731, 732, 733, 295, 295, 621, 622, 300, 300, 744, 623, 747,
    624, 625, 628, 752, 224, 225, 631, 634, 226, 227, 635, 637, 660, 760, 639, 762, 763, 145, 146,
    147, 640, 148, 149, 150, 228, 229, 645, 646, 647, 648, 677, 230, 231, 778, 649, 650, 780, 151,
    152, 153, 651, 652, 653, 654, 154, 155, 156, 657, 661, 787, 663, 469, 667, 674, 680, 686, 697,
    699, 709, 700, 701, 711, 702, 704, 485, 486, 487, 798, 708, 714, 725, 718, 232, 719, 721, 724,
    802, 803, 723, 727, 728, 729, 502, 745, 748, 735, 736, 740, 742, 761, 248, 751, 753, 522, 523,
    754, 525, 755, 756, 757, 758, 564, 759, 764, 779, 765, 582, 706, 559, 766, 767, 240, 768, 769,
    788, 770, 771, 772, 582, 773, 774, 789, 553, 157, 158, 159, 160, 775, 776, 777, 783, 784, 785,
    786, 790, 565, 566, 567, 791, 792, 793, 394, 395, 396, 397, 398, 31,  32,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  399, 794, 795, 400, 401, 402, 403, 404, 405,
    406, 796, 797, 800, 407, 801, 804, 805, 806, 380, 408, 409, 410, 807, 808, 411, 412, 413, 414,
    415, 809, 810, 811, 812, 416, 417, 418, 419, 781, 750, 582, 420, 421, 422, 423, 424, 425, 426,
    713, 427, 428, 429, 703, 717, 430, 431, 432, 433, 434, 435, 436, 561, 0,   437, 438, 439, 440,
    441, 442, 0,   443, 444, 470, 0,   0,   0,   0,   0,   0,   0,   445, 446, 447, 448, 449, 450,
    451, 0,   452, 453, 454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 246, 247, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   582, 0,   0,   0,   60,  61,  62,  63,  64,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  65,  0,   0,   66,  67,  68,  69,  70,  71,  72,  0,   0,
    0,   73,  0,   0,   0,   582, 74,  75,  76,  77,  0,   0,   78,  79,  48,  80,  81,  0,   0,
    0,   0,   82,  83,  84,  85,  0,   0,   0,   86,  87,  88,  89,  90,  91,  92,  0,   93,  94,
    95,  0,   0,   96,  97,  98,  99,  100, 101, 102, 0,   0,   103, 104, 105, 106, 107, 108, 0,
    109, 110, 111, 112, 113, 114, 115, 116, 0,   0,   117, 118, 119, 120, 121, 122, 123, 0,   124,
    125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 51,  60,  61,  62,  63,  64,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  65,  0,   0,   66,
    67,  68,  69,  70,  71,  72,  0,   0,   0,   73,  0,   0,   0,   0,   705, 75,  76,  77,  0,
    0,   78,  79,  48,  80,  81,  0,   0,   0,   0,   82,  83,  84,  85,  0,   0,   0,   86,  87,
    88,  89,  90,  91,  92,  0,   93,  94,  95,  0,   0,   96,  97,  98,  99,  100, 101, 102, 0,
    0,   103, 104, 105, 106, 107, 108, 0,   109, 110, 111, 112, 113, 114, 115, 116, 0,   0,   117,
    118, 119, 120, 121, 122, 123, 0,   124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 51,  172, 173, 0,   0,   0,   0,   0,   0,   0,   145, 146, 147, 0,   148, 149, 150, 701,
    0,   0,   0,   0,   174, 175, 0,   0,   0,   0,   0,   176, 151, 152, 153, 0,   0,   0,   0,
    154, 155, 156, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   177, 178, 0,   0,   0,
    0,   0,   0,   0,   179, 180, 0,   0,   0,   0,   0,   0,   181, 0,   0,   0,   0,   0,   0,
    0,   0,   277, 388, 0,   0,   0,   0,   0,   0,   0,   183, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   184, 185, 186, 187, 188, 189, 190, 191, 192, 193,
    194, 195, 196, 157, 158, 159, 160, 197, 198, 199, 172, 173, 0,   0,   0,   0,   0,   0,   0,
    145, 146, 147, 0,   148, 149, 150, 0,   0,   0,   0,   0,   174, 175, 0,   0,   0,   0,   0,
    176, 151, 152, 153, 0,   0,   0,   0,   154, 155, 156, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   177, 178, 0,   0,   0,   0,   0,   0,   0,   179, 180, 0,   0,   0,   0,   0,
    0,   181, 0,   0,   0,   0,   0,   0,   0,   0,   277, 388, 0,   0,   0,   0,   0,   0,   0,
    183, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   184, 185,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 157, 158, 159, 160, 197, 198, 199, 172,
    173, 0,   0,   0,   0,   0,   0,   0,   145, 146, 147, 0,   148, 149, 150, 0,   0,   0,   0,
    0,   174, 175, 0,   0,   0,   0,   0,   176, 151, 152, 153, 0,   0,   0,   0,   154, 155, 156,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   177, 178, 0,   0,   0,   0,   0,   0,
    0,   179, 180, 0,   0,   0,   0,   0,   0,   181, 0,   0,   0,   0,   0,   0,   0,   0,   579,
    580, 0,   0,   0,   0,   0,   0,   0,   183, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196,
    157, 158, 159, 160, 197, 198, 199, 172, 173, 0,   0,   0,   0,   0,   0,   0,   145, 146, 147,
    0,   148, 149, 150, 0,   0,   0,   0,   0,   174, 175, 0,   0,   0,   0,   0,   176, 151, 152,
    153, 0,   0,   0,   0,   154, 155, 156, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    177, 178, 0,   0,   0,   0,   0,   0,   0,   179, 180, 0,   0,   0,   0,   0,   0,   181, 0,
    0,   0,   0,   0,   0,   0,   0,   0,   182, 0,   0,   0,   0,   0,   0,   0,   183, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   184, 185, 186, 187, 188,
    189, 190, 191, 192, 193, 194, 195, 196, 157, 158, 159, 160, 197, 198, 199, 263, 264, 0,   0,
    0,   0,   0,   0,   0,   265, 266, 267, 0,   268, 269, 270, 0,   0,   0,   0,   0,   174, 175,
    0,   0,   0,   0,   0,   176, 271, 272, 273, 0,   0,   0,   0,   274, 275, 276, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   177, 178, 0,   0,   0,   0,   0,   0,   0,   179, 180,
    0,   0,   0,   0,   0,   0,   181, 0,   0,   0,   0,   0,   0,   0,   0,   277, 278, 0,   0,
    0,   0,   0,   0,   0,   183, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   184, 0,   0,   187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 279, 280, 281,
    282, 197, 198, 199, 394, 395, 396, 397, 398, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   399, 0,   0,   400, 401, 402, 403, 404, 405, 406, 0,   0,   0,
    407, 0,   0,   0,   0,   0,   408, 409, 410, 0,   0,   411, 412, 0,   414, 415, 0,   0,   0,
    0,   416, 417, 418, 419, 0,   0,   0,   420, 421, 422, 423, 424, 425, 426, 0,   427, 428, 429,
    0,   0,   430, 431, 432, 433, 434, 435, 436, 0,   0,   437, 438, 439, 440, 441, 442, 0,   443,
    444, 0,   0,   0,   0,   0,   0,   0,   0,   445, 446, 447, 448, 449, 450, 451, 0,   452, 453,
    454, 455, 456, 457, 458, 459, 460, 461, 462, 463, 464, 30,  0,   31,  32,  33,  34,  35,  36,
    37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  31,  32,  33,  34,  35,  36,  37,  38,  39,
    40,  41,  42,  43,  44,  45,  46,  0,   0,   47,  0,   0,   0,   0,   0,   0,   0,   48,  0,
    0,   0,   0,   0,   0,   0,   244, 0,   0,   0,   0,   0,   0,   0,   245, 0,   0,   0,   0,
    49,  0,   50,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    0,   31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  0,   558,
    0,   0,   0,   0,   0,   0,   0,   413, 0,   0,   0,   0,   0,   0,   0,   51,  562, 0,   0,
    0,   0,   0,   0,   0,   48,  0,   0,   0,   0,   0,   0,   246, 247, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   111, 112, 113, 114, 115, 116, 0,
    0,   0,   0,   0,   0,   246, 247, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   51};

const short ParserGen::yycheck_[] = {
    55,  14,  14,  14,  21,  394, 236, 245, 101, 17,  248, 400, 20,  245, 390, 75,  248, 102, 256,
    17,  409, 410, 20,  484, 223, 20,  102, 41,  417, 238, 419, 17,  465, 466, 20,  496, 245, 0,
    245, 248, 245, 248, 245, 248, 245, 248, 525, 248, 278, 245, 110, 245, 248, 245, 248, 444, 248,
    446, 447, 245, 259, 750, 248, 452, 453, 454, 455, 456, 457, 458, 459, 460, 461, 245, 553, 464,
    248, 395, 245, 397, 17,  248, 102, 20,  38,  39,  40,  476, 102, 478, 479, 42,  781, 401, 245,
    42,  102, 248, 245, 488, 408, 248, 41,  411, 412, 494, 495, 483, 102, 498, 428, 102, 420, 421,
    102, 135, 431, 432, 426, 42,  101, 72,  42,  512, 513, 72,  515, 21,  517, 437, 46,  47,  440,
    9,   442, 524, 102, 53,  527, 528, 67,  530, 531, 42,  18,  534, 42,  536, 72,  41,  539, 540,
    541, 542, 147, 148, 149, 46,  47,  75,  76,  101, 42,  101, 53,  554, 101, 101, 84,  85,  101,
    101, 101, 101, 563, 101, 92,  95,  96,  97,  98,  99,  100, 42,  101, 574, 75,  76,  142, 101,
    101, 101, 101, 101, 110, 84,  85,  586, 587, 588, 589, 101, 42,  92,  101, 42,  595, 596, 101,
    42,  599, 127, 42,  101, 603, 101, 101, 101, 101, 101, 101, 110, 101, 101, 42,  614, 615, 465,
    466, 237, 619, 14,  621, 465, 466, 624, 625, 626, 468, 237, 629, 630, 237, 632, 633, 706, 11,
    636, 10,  638, 42,  237, 641, 642, 643, 644, 465, 466, 465, 466, 465, 466, 465, 466, 465, 466,
    42,  656, 42,  465, 466, 465, 466, 465, 466, 42,  42,  8,   42,  465, 466, 42,  42,  672, 673,
    42,  42,  42,  677, 42,  42,  42,  42,  465, 466, 42,  42,  686, 465, 466, 237, 690, 42,  679,
    693, 694, 695, 42,  697, 42,  42,  687, 688, 689, 465, 466, 14,  42,  465, 466, 709, 42,  711,
    12,  12,  42,  715, 34,  35,  42,  42,  38,  39,  42,  42,  41,  725, 42,  727, 728, 34,  35,
    36,  42,  38,  39,  40,  54,  55,  42,  42,  42,  42,  14,  61,  62,  745, 42,  42,  748, 54,
    55,  56,  42,  42,  42,  42,  61,  62,  63,  42,  41,  761, 41,  382, 41,  41,  41,  14,  14,
    42,  15,  42,  41,  13,  42,  41,  404, 405, 406, 779, 42,  41,  17,  42,  102, 41,  41,  41,
    788, 789, 42,  14,  14,  41,  422, 16,  22,  41,  41,  41,  41,  19,  236, 42,  41,  433, 434,
    41,  436, 42,  42,  42,  41,  471, 42,  42,  23,  42,  484, 666, 468, 41,  41,  171, 41,  41,
    20,  42,  42,  41,  496, 41,  41,  20,  462, 140, 141, 142, 143, 42,  42,  42,  42,  42,  42,
    42,  42,  471, 471, 471, 42,  42,  42,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  42,  42,  27,  28,  29,  30,  31,  32,
    33,  42,  42,  42,  37,  42,  42,  42,  42,  248, 43,  44,  45,  42,  42,  48,  49,  50,  51,
    52,  42,  42,  42,  42,  57,  58,  59,  60,  749, 713, 579, 64,  65,  66,  67,  68,  69,  70,
    670, 72,  73,  74,  664, 673, 77,  78,  79,  80,  81,  82,  83,  470, -1,  86,  87,  88,  89,
    90,  91,  -1,  93,  94,  383, -1,  -1,  -1,  -1,  -1,  -1,  -1,  103, 104, 105, 106, 107, 108,
    109, -1,  111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  664, -1,  -1,  -1,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  24,  -1,  -1,  27,  28,  29,  30,  31,  32,  33,  -1,  -1,
    -1,  37,  -1,  -1,  -1,  706, 42,  43,  44,  45,  -1,  -1,  48,  49,  50,  51,  52,  -1,  -1,
    -1,  -1,  57,  58,  59,  60,  -1,  -1,  -1,  64,  65,  66,  67,  68,  69,  70,  -1,  72,  73,
    74,  -1,  -1,  77,  78,  79,  80,  81,  82,  83,  -1,  -1,  86,  87,  88,  89,  90,  91,  -1,
    93,  94,  95,  96,  97,  98,  99,  100, -1,  -1,  103, 104, 105, 106, 107, 108, 109, -1,  111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 3,   4,   5,   6,   7,   8,
    9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  -1,  -1,  27,
    28,  29,  30,  31,  32,  33,  -1,  -1,  -1,  37,  -1,  -1,  -1,  -1,  42,  43,  44,  45,  -1,
    -1,  48,  49,  50,  51,  52,  -1,  -1,  -1,  -1,  57,  58,  59,  60,  -1,  -1,  -1,  64,  65,
    66,  67,  68,  69,  70,  -1,  72,  73,  74,  -1,  -1,  77,  78,  79,  80,  81,  82,  83,  -1,
    -1,  86,  87,  88,  89,  90,  91,  -1,  93,  94,  95,  96,  97,  98,  99,  100, -1,  -1,  103,
    104, 105, 106, 107, 108, 109, -1,  111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
    123, 124, 25,  26,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  34,  35,  36,  -1,  38,  39,  40,  41,
    -1,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,  56,  -1,  -1,  -1,  -1,
    61,  62,  63,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  84,  85,  -1,  -1,  -1,  -1,  -1,  -1,  92,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  101, 102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  110, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  127, 128, 129, 130, 131, 132, 133, 134, 135, 136,
    137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 25,  26,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    34,  35,  36,  -1,  38,  39,  40,  -1,  -1,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,  -1,  -1,
    53,  54,  55,  56,  -1,  -1,  -1,  -1,  61,  62,  63,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  75,  76,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  84,  85,  -1,  -1,  -1,  -1,  -1,
    -1,  92,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  101, 102, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    110, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  127, 128,
    129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 25,
    26,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  34,  35,  36,  -1,  38,  39,  40,  -1,  -1,  -1,  -1,
    -1,  46,  47,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,  56,  -1,  -1,  -1,  -1,  61,  62,  63,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  84,  85,  -1,  -1,  -1,  -1,  -1,  -1,  92,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  101,
    102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  110, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139,
    140, 141, 142, 143, 144, 145, 146, 25,  26,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  34,  35,  36,
    -1,  38,  39,  40,  -1,  -1,  -1,  -1,  -1,  46,  47,  -1,  -1,  -1,  -1,  -1,  53,  54,  55,
    56,  -1,  -1,  -1,  -1,  61,  62,  63,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    75,  76,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  84,  85,  -1,  -1,  -1,  -1,  -1,  -1,  92,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  102, -1,  -1,  -1,  -1,  -1,  -1,  -1,  110, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  127, 128, 129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 25,  26,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  34,  35,  36,  -1,  38,  39,  40,  -1,  -1,  -1,  -1,  -1,  46,  47,
    -1,  -1,  -1,  -1,  -1,  53,  54,  55,  56,  -1,  -1,  -1,  -1,  61,  62,  63,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  75,  76,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  84,  85,
    -1,  -1,  -1,  -1,  -1,  -1,  92,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  101, 102, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  110, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  127, -1,  -1,  130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
    143, 144, 145, 146, 3,   4,   5,   6,   7,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  24,  -1,  -1,  27,  28,  29,  30,  31,  32,  33,  -1,  -1,  -1,
    37,  -1,  -1,  -1,  -1,  -1,  43,  44,  45,  -1,  -1,  48,  49,  -1,  51,  52,  -1,  -1,  -1,
    -1,  57,  58,  59,  60,  -1,  -1,  -1,  64,  65,  66,  67,  68,  69,  70,  -1,  72,  73,  74,
    -1,  -1,  77,  78,  79,  80,  81,  82,  83,  -1,  -1,  86,  87,  88,  89,  90,  91,  -1,  93,
    94,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  103, 104, 105, 106, 107, 108, 109, -1,  111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 6,   -1,  8,   9,   10,  11,  12,  13,
    14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  22,  23,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  50,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  42,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,
    71,  -1,  73,  8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,
    -1,  8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  -1,  42,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  124, 42,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  50,  -1,  -1,  -1,  -1,  -1,  -1,  124, 125, -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  95,  96,  97,  98,  99,  100, -1,
    -1,  -1,  -1,  -1,  -1,  124, 125, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  124};

const short ParserGen::yystos_[] = {
    0,   147, 148, 149, 321, 101, 195, 102, 305, 102, 315, 0,   102, 196, 306, 316, 95,  96,  97,
    98,  99,  100, 197, 198, 199, 200, 201, 202, 203, 41,  6,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  42,  50,  71,  73,  124, 155, 158, 159, 162, 167,
    310, 311, 314, 3,   4,   5,   6,   7,   24,  27,  28,  29,  30,  31,  32,  33,  37,  42,  43,
    44,  45,  48,  49,  51,  52,  57,  58,  59,  60,  64,  65,  66,  67,  68,  69,  70,  72,  73,
    74,  77,  78,  79,  80,  81,  82,  83,  86,  87,  88,  89,  90,  91,  93,  94,  95,  96,  97,
    98,  99,  100, 103, 104, 105, 106, 107, 108, 109, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 154, 155, 157, 158, 159, 161, 320, 102, 34,  35,  36,  38,  39,  40,  54,
    55,  56,  61,  62,  63,  140, 141, 142, 143, 172, 174, 175, 176, 209, 102, 102, 209, 322, 323,
    42,  25,  26,  46,  47,  53,  75,  76,  84,  85,  92,  102, 110, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 144, 145, 146, 168, 169, 170, 171, 172, 173, 174, 175, 176,
    177, 178, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 194, 308, 101, 34,  35,  38,  39,
    54,  55,  61,  62,  102, 317, 318, 42,  204, 21,  9,   102, 196, 307, 305, 67,  42,  50,  124,
    125, 151, 152, 156, 163, 167, 209, 180, 42,  72,  312, 313, 309, 75,  110, 319, 25,  26,  34,
    35,  36,  38,  39,  40,  54,  55,  56,  61,  62,  63,  101, 102, 140, 141, 142, 143, 168, 169,
    170, 171, 173, 177, 178, 180, 182, 183, 184, 186, 187, 188, 206, 207, 208, 211, 214, 215, 216,
    219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237,
    238, 239, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257,
    258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 268, 269, 270, 271, 272, 273, 274, 275, 276,
    277, 278, 279, 280, 281, 282, 283, 284, 285, 286, 287, 297, 298, 299, 300, 301, 302, 303, 304,
    206, 42,  18,  102, 188, 41,  305, 42,  102, 189, 210, 211, 217, 294, 3,   4,   5,   6,   7,
    24,  27,  28,  29,  30,  31,  32,  33,  37,  43,  44,  45,  48,  49,  50,  51,  52,  57,  58,
    59,  60,  64,  65,  66,  67,  68,  69,  70,  72,  73,  74,  77,  78,  79,  80,  81,  82,  83,
    86,  87,  88,  89,  90,  91,  93,  94,  103, 104, 105, 106, 107, 108, 109, 111, 112, 113, 114,
    115, 116, 117, 118, 119, 120, 121, 122, 123, 151, 160, 164, 205, 175, 312, 218, 294, 41,  210,
    216, 101, 216, 101, 101, 212, 210, 212, 101, 101, 322, 322, 322, 101, 212, 210, 210, 212, 212,
    101, 101, 101, 210, 101, 210, 212, 212, 322, 46,  47,  53,  75,  76,  84,  85,  92,  110, 101,
    101, 212, 101, 216, 101, 240, 322, 240, 240, 322, 322, 101, 322, 212, 101, 101, 212, 101, 101,
    212, 213, 101, 210, 101, 210, 210, 101, 101, 101, 101, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 322, 101, 210, 207, 207, 42,  164, 42,  307, 42,  153, 154, 155, 158, 159, 165, 42,
    42,  210, 42,  210, 210, 42,  42,  42,  294, 101, 102, 179, 189, 190, 191, 192, 14,  11,  10,
    210, 42,  42,  42,  42,  42,  210, 210, 179, 42,  210, 42,  42,  42,  8,   292, 42,  42,  42,
    42,  42,  42,  42,  42,  42,  210, 210, 42,  210, 42,  210, 42,  14,  42,  42,  12,  12,  210,
    292, 42,  210, 210, 42,  210, 210, 42,  42,  210, 42,  210, 42,  42,  210, 210, 210, 210, 42,
    42,  42,  42,  42,  42,  42,  42,  42,  42,  292, 210, 42,  42,  210, 41,  41,  210, 41,  179,
    295, 193, 41,  210, 210, 210, 210, 210, 210, 41,  210, 210, 14,  210, 210, 41,  210, 210, 210,
    210, 210, 14,  210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 14,  210, 42,  42,  41,  42,
    295, 41,  42,  161, 166, 42,  15,  288, 13,  290, 290, 41,  210, 296, 296, 42,  41,  210, 41,
    294, 42,  41,  17,  293, 14,  14,  41,  210, 294, 294, 294, 210, 41,  41,  210, 210, 210, 41,
    210, 41,  179, 210, 16,  289, 210, 22,  291, 291, 42,  210, 41,  41,  42,  42,  42,  41,  42,
    210, 19,  210, 210, 42,  42,  41,  41,  41,  41,  42,  42,  41,  41,  41,  42,  42,  42,  210,
    23,  210, 288, 289, 42,  42,  42,  42,  210, 20,  20,  42,  42,  42,  42,  42,  42,  42,  42,
    210, 289, 42,  42,  210, 210, 42,  42,  42,  42,  42,  42,  42,  42,  42};

const short ParserGen::yyr1_[] = {
    0,   150, 321, 321, 321, 195, 196, 196, 323, 322, 197, 197, 197, 197, 197, 197, 203, 198, 199,
    209, 209, 209, 209, 200, 201, 202, 204, 204, 163, 163, 206, 207, 207, 207, 207, 207, 207, 207,
    207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207, 207,
    207, 207, 207, 207, 207, 207, 207, 207, 151, 152, 152, 152, 208, 205, 205, 164, 164, 305, 306,
    306, 310, 310, 308, 308, 307, 307, 312, 313, 313, 311, 314, 314, 314, 309, 309, 162, 162, 162,
    158, 154, 154, 154, 154, 154, 154, 155, 156, 167, 167, 167, 167, 167, 167, 167, 167, 167, 167,
    167, 167, 167, 167, 167, 167, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157,
    157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157,
    157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157,
    157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157, 157,
    180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 181, 194, 182, 183, 184, 186, 187, 188, 168,
    169, 170, 171, 173, 177, 178, 172, 172, 172, 172, 174, 174, 174, 174, 175, 175, 175, 175, 176,
    176, 176, 176, 185, 185, 189, 189, 189, 189, 189, 189, 189, 189, 189, 189, 189, 189, 189, 189,
    189, 189, 189, 189, 189, 189, 189, 294, 294, 210, 210, 210, 212, 213, 211, 211, 211, 211, 211,
    211, 211, 211, 211, 211, 214, 215, 215, 216, 217, 218, 218, 165, 153, 153, 153, 153, 159, 160,
    219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 219, 220, 220,
    220, 220, 220, 220, 220, 220, 220, 221, 222, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282,
    283, 284, 285, 286, 287, 223, 223, 223, 224, 225, 226, 230, 230, 230, 230, 230, 230, 230, 230,
    230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 230, 231, 290, 290, 291, 291,
    232, 233, 296, 296, 296, 234, 235, 292, 292, 236, 243, 253, 293, 293, 240, 237, 238, 239, 241,
    242, 244, 245, 246, 247, 248, 249, 250, 251, 252, 319, 319, 317, 315, 316, 316, 318, 318, 318,
    318, 318, 318, 318, 318, 320, 320, 297, 297, 297, 297, 297, 297, 297, 298, 299, 300, 301, 302,
    303, 304, 227, 227, 228, 229, 179, 179, 190, 190, 191, 295, 295, 192, 193, 193, 166, 161, 161,
    161, 161, 161, 254, 254, 254, 254, 254, 254, 254, 255, 256, 257, 258, 259, 260, 261, 262, 262,
    262, 262, 262, 262, 262, 262, 262, 262, 288, 288, 289, 289, 263, 264, 265, 266, 267, 268, 269,
    270, 271, 272};

const signed char ParserGen::yyr2_[] = {
    0, 2,  2,  2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3, 7,  1,  1,  1, 1, 2, 2, 4, 0, 2, 2, 2,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 3, 1, 2, 2, 2, 3, 0, 2, 2, 1,  1,  3,  0, 2, 1, 2, 5, 5, 1, 1, 1,
    0, 2,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 1, 1, 1,  4,  5,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  4,  4, 3, 3, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1,  4,  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 7, 4, 4, 4,  7,  4,  7, 8, 7, 7, 4, 7, 7, 1, 1,
    1, 4,  4,  6, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 6, 0, 2, 0,
    2, 11, 10, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4,  11, 11, 7, 4, 4, 7, 8, 8, 8, 4, 4,
    1, 1,  4,  3, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1,  1,  1,  1, 1, 6, 6, 4, 8, 8, 4, 8,
    1, 1,  6,  6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4, 4,
    4, 4,  4,  4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4,  4,  4, 4, 4, 4, 4, 4, 4};


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
    0,    331,  331,  334,  337,  344,  350,  351,  359,  359,  362,  362,  362,  362,  362,  362,
    365,  375,  381,  391,  391,  391,  391,  395,  400,  405,  424,  427,  434,  437,  443,  457,
    458,  459,  460,  461,  462,  463,  464,  465,  466,  467,  468,  471,  474,  477,  480,  483,
    486,  489,  492,  495,  498,  501,  504,  507,  510,  513,  516,  519,  522,  523,  524,  525,
    526,  531,  539,  552,  553,  570,  577,  581,  589,  592,  598,  604,  607,  613,  616,  625,
    626,  632,  635,  642,  645,  649,  658,  666,  667,  668,  671,  674,  681,  681,  681,  684,
    692,  695,  698,  701,  704,  707,  713,  719,  738,  741,  744,  747,  750,  753,  756,  759,
    762,  765,  768,  771,  774,  777,  780,  783,  791,  794,  797,  800,  803,  806,  809,  812,
    815,  818,  821,  824,  827,  830,  833,  836,  839,  842,  845,  848,  851,  854,  857,  860,
    863,  866,  869,  872,  875,  878,  881,  884,  887,  890,  893,  896,  899,  902,  905,  908,
    911,  914,  917,  920,  923,  926,  929,  932,  935,  938,  941,  944,  947,  950,  953,  956,
    959,  962,  965,  968,  971,  974,  977,  980,  983,  986,  989,  992,  995,  998,  1005, 1010,
    1013, 1016, 1019, 1022, 1025, 1028, 1031, 1034, 1040, 1054, 1068, 1074, 1080, 1086, 1092, 1098,
    1104, 1110, 1116, 1122, 1128, 1134, 1140, 1146, 1149, 1152, 1155, 1161, 1164, 1167, 1170, 1176,
    1179, 1182, 1185, 1191, 1194, 1197, 1200, 1206, 1209, 1215, 1216, 1217, 1218, 1219, 1220, 1221,
    1222, 1223, 1224, 1225, 1226, 1227, 1228, 1229, 1230, 1231, 1232, 1233, 1234, 1235, 1242, 1243,
    1250, 1250, 1250, 1255, 1262, 1268, 1268, 1268, 1268, 1268, 1269, 1269, 1269, 1269, 1269, 1273,
    1277, 1281, 1290, 1298, 1304, 1307, 1314, 1321, 1321, 1321, 1321, 1325, 1331, 1337, 1337, 1337,
    1337, 1337, 1337, 1337, 1337, 1337, 1337, 1337, 1337, 1337, 1338, 1338, 1338, 1338, 1342, 1345,
    1348, 1351, 1354, 1357, 1360, 1363, 1366, 1372, 1379, 1385, 1390, 1395, 1401, 1406, 1411, 1416,
    1422, 1427, 1433, 1442, 1448, 1454, 1459, 1465, 1471, 1471, 1471, 1475, 1482, 1489, 1496, 1496,
    1496, 1496, 1496, 1496, 1496, 1497, 1497, 1497, 1497, 1497, 1497, 1497, 1497, 1498, 1498, 1498,
    1498, 1498, 1498, 1498, 1502, 1512, 1515, 1521, 1524, 1530, 1539, 1548, 1551, 1554, 1560, 1571,
    1582, 1585, 1591, 1599, 1607, 1615, 1618, 1623, 1632, 1638, 1644, 1650, 1660, 1670, 1677, 1684,
    1691, 1699, 1707, 1715, 1723, 1729, 1735, 1738, 1744, 1750, 1755, 1758, 1765, 1768, 1771, 1774,
    1777, 1780, 1783, 1786, 1791, 1793, 1799, 1799, 1799, 1799, 1799, 1799, 1800, 1804, 1810, 1816,
    1823, 1834, 1845, 1852, 1863, 1863, 1867, 1874, 1881, 1881, 1885, 1885, 1889, 1895, 1896, 1903,
    1909, 1912, 1919, 1926, 1927, 1928, 1929, 1930, 1933, 1933, 1933, 1933, 1933, 1933, 1933, 1935,
    1940, 1945, 1950, 1955, 1960, 1965, 1971, 1972, 1973, 1974, 1975, 1976, 1977, 1978, 1979, 1980,
    1985, 1988, 1995, 1998, 2004, 2014, 2019, 2024, 2029, 2034, 2039, 2044, 2049, 2054};

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
#line 6835 "src/mongo/db/cst/parser_gen.cpp"

#line 2058 "src/mongo/db/cst/grammar.yy"
