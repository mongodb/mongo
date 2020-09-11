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

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
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

#line 68 "parser_gen.cpp"


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
#line 161 "parser_gen.cpp"

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

        case symbol_kind::S_dbPointer:            // dbPointer
        case symbol_kind::S_javascript:           // javascript
        case symbol_kind::S_symbol:               // symbol
        case symbol_kind::S_javascriptWScope:     // javascriptWScope
        case symbol_kind::S_int:                  // int
        case symbol_kind::S_timestamp:            // timestamp
        case symbol_kind::S_long:                 // long
        case symbol_kind::S_double:               // double
        case symbol_kind::S_decimal:              // decimal
        case symbol_kind::S_minKey:               // minKey
        case symbol_kind::S_maxKey:               // maxKey
        case symbol_kind::S_value:                // value
        case symbol_kind::S_string:               // string
        case symbol_kind::S_fieldPath:            // fieldPath
        case symbol_kind::S_binary:               // binary
        case symbol_kind::S_undefined:            // undefined
        case symbol_kind::S_objectId:             // objectId
        case symbol_kind::S_bool:                 // bool
        case symbol_kind::S_date:                 // date
        case symbol_kind::S_null:                 // null
        case symbol_kind::S_regex:                // regex
        case symbol_kind::S_simpleValue:          // simpleValue
        case symbol_kind::S_compoundValue:        // compoundValue
        case symbol_kind::S_valueArray:           // valueArray
        case symbol_kind::S_valueObject:          // valueObject
        case symbol_kind::S_valueFields:          // valueFields
        case symbol_kind::S_variable:             // variable
        case symbol_kind::S_pipeline:             // pipeline
        case symbol_kind::S_stageList:            // stageList
        case symbol_kind::S_stage:                // stage
        case symbol_kind::S_inhibitOptimization:  // inhibitOptimization
        case symbol_kind::S_unionWith:            // unionWith
        case symbol_kind::S_skip:                 // skip
        case symbol_kind::S_limit:                // limit
        case symbol_kind::S_project:              // project
        case symbol_kind::S_sample:               // sample
        case symbol_kind::S_projectFields:        // projectFields
        case symbol_kind::S_projection:           // projection
        case symbol_kind::S_num:                  // num
        case symbol_kind::S_expression:           // expression
        case symbol_kind::S_compoundExpression:   // compoundExpression
        case symbol_kind::S_exprFixedTwoArg:      // exprFixedTwoArg
        case symbol_kind::S_expressionArray:      // expressionArray
        case symbol_kind::S_expressionObject:     // expressionObject
        case symbol_kind::S_expressionFields:     // expressionFields
        case symbol_kind::S_maths:                // maths
        case symbol_kind::S_add:                  // add
        case symbol_kind::S_atan2:                // atan2
        case symbol_kind::S_boolExps:             // boolExps
        case symbol_kind::S_and:                  // and
        case symbol_kind::S_or:                   // or
        case symbol_kind::S_not:                  // not
        case symbol_kind::S_literalEscapes:       // literalEscapes
        case symbol_kind::S_const:                // const
        case symbol_kind::S_literal:              // literal
        case symbol_kind::S_stringExps:           // stringExps
        case symbol_kind::S_concat:               // concat
        case symbol_kind::S_dateFromString:       // dateFromString
        case symbol_kind::S_dateToString:         // dateToString
        case symbol_kind::S_indexOfBytes:         // indexOfBytes
        case symbol_kind::S_indexOfCP:            // indexOfCP
        case symbol_kind::S_ltrim:                // ltrim
        case symbol_kind::S_regexFind:            // regexFind
        case symbol_kind::S_regexFindAll:         // regexFindAll
        case symbol_kind::S_regexMatch:           // regexMatch
        case symbol_kind::S_regexArgs:            // regexArgs
        case symbol_kind::S_replaceOne:           // replaceOne
        case symbol_kind::S_replaceAll:           // replaceAll
        case symbol_kind::S_rtrim:                // rtrim
        case symbol_kind::S_split:                // split
        case symbol_kind::S_strLenBytes:          // strLenBytes
        case symbol_kind::S_strLenCP:             // strLenCP
        case symbol_kind::S_strcasecmp:           // strcasecmp
        case symbol_kind::S_substr:               // substr
        case symbol_kind::S_substrBytes:          // substrBytes
        case symbol_kind::S_substrCP:             // substrCP
        case symbol_kind::S_toLower:              // toLower
        case symbol_kind::S_toUpper:              // toUpper
        case symbol_kind::S_trim:                 // trim
        case symbol_kind::S_compExprs:            // compExprs
        case symbol_kind::S_cmp:                  // cmp
        case symbol_kind::S_eq:                   // eq
        case symbol_kind::S_gt:                   // gt
        case symbol_kind::S_gte:                  // gte
        case symbol_kind::S_lt:                   // lt
        case symbol_kind::S_lte:                  // lte
        case symbol_kind::S_ne:                   // ne
        case symbol_kind::S_typeExpression:       // typeExpression
        case symbol_kind::S_convert:              // convert
        case symbol_kind::S_toBool:               // toBool
        case symbol_kind::S_toDate:               // toDate
        case symbol_kind::S_toDecimal:            // toDecimal
        case symbol_kind::S_toDouble:             // toDouble
        case symbol_kind::S_toInt:                // toInt
        case symbol_kind::S_toLong:               // toLong
        case symbol_kind::S_toObjectId:           // toObjectId
        case symbol_kind::S_toString:             // toString
        case symbol_kind::S_type:                 // type
        case symbol_kind::S_abs:                  // abs
        case symbol_kind::S_ceil:                 // ceil
        case symbol_kind::S_divide:               // divide
        case symbol_kind::S_exponent:             // exponent
        case symbol_kind::S_floor:                // floor
        case symbol_kind::S_ln:                   // ln
        case symbol_kind::S_log:                  // log
        case symbol_kind::S_logten:               // logten
        case symbol_kind::S_mod:                  // mod
        case symbol_kind::S_multiply:             // multiply
        case symbol_kind::S_pow:                  // pow
        case symbol_kind::S_round:                // round
        case symbol_kind::S_sqrt:                 // sqrt
        case symbol_kind::S_subtract:             // subtract
        case symbol_kind::S_trunc:                // trunc
        case symbol_kind::S_setExpression:        // setExpression
        case symbol_kind::S_allElementsTrue:      // allElementsTrue
        case symbol_kind::S_anyElementTrue:       // anyElementTrue
        case symbol_kind::S_setDifference:        // setDifference
        case symbol_kind::S_setEquals:            // setEquals
        case symbol_kind::S_setIntersection:      // setIntersection
        case symbol_kind::S_setIsSubset:          // setIsSubset
        case symbol_kind::S_setUnion:             // setUnion
        case symbol_kind::S_match:                // match
        case symbol_kind::S_predicates:           // predicates
        case symbol_kind::S_compoundMatchExprs:   // compoundMatchExprs
        case symbol_kind::S_predValue:            // predValue
        case symbol_kind::S_additionalExprs:      // additionalExprs
        case symbol_kind::S_sortSpecs:            // sortSpecs
        case symbol_kind::S_specList:             // specList
        case symbol_kind::S_metaSort:             // metaSort
        case symbol_kind::S_oneOrNegOne:          // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:      // metaSortKeyword
            value.YY_MOVE_OR_COPY<CNode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_projectionFieldname:      // projectionFieldname
        case symbol_kind::S_expressionFieldname:      // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
        case symbol_kind::S_predFieldname:            // predFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
        case symbol_kind::S_logicalExprField:         // logicalExprField
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

        case symbol_kind::S_projectField:        // projectField
        case symbol_kind::S_expressionField:     // expressionField
        case symbol_kind::S_valueField:          // valueField
        case symbol_kind::S_onErrorArg:          // onErrorArg
        case symbol_kind::S_onNullArg:           // onNullArg
        case symbol_kind::S_formatArg:           // formatArg
        case symbol_kind::S_timezoneArg:         // timezoneArg
        case symbol_kind::S_charsArg:            // charsArg
        case symbol_kind::S_optionsArg:          // optionsArg
        case symbol_kind::S_predicate:           // predicate
        case symbol_kind::S_logicalExpr:         // logicalExpr
        case symbol_kind::S_operatorExpression:  // operatorExpression
        case symbol_kind::S_notExpr:             // notExpr
        case symbol_kind::S_sortSpec:            // sortSpec
            value.YY_MOVE_OR_COPY<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
            value.YY_MOVE_OR_COPY<std::string>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
            value.YY_MOVE_OR_COPY<std::vector<CNode>>(YY_MOVE(that.value));
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

        case symbol_kind::S_dbPointer:            // dbPointer
        case symbol_kind::S_javascript:           // javascript
        case symbol_kind::S_symbol:               // symbol
        case symbol_kind::S_javascriptWScope:     // javascriptWScope
        case symbol_kind::S_int:                  // int
        case symbol_kind::S_timestamp:            // timestamp
        case symbol_kind::S_long:                 // long
        case symbol_kind::S_double:               // double
        case symbol_kind::S_decimal:              // decimal
        case symbol_kind::S_minKey:               // minKey
        case symbol_kind::S_maxKey:               // maxKey
        case symbol_kind::S_value:                // value
        case symbol_kind::S_string:               // string
        case symbol_kind::S_fieldPath:            // fieldPath
        case symbol_kind::S_binary:               // binary
        case symbol_kind::S_undefined:            // undefined
        case symbol_kind::S_objectId:             // objectId
        case symbol_kind::S_bool:                 // bool
        case symbol_kind::S_date:                 // date
        case symbol_kind::S_null:                 // null
        case symbol_kind::S_regex:                // regex
        case symbol_kind::S_simpleValue:          // simpleValue
        case symbol_kind::S_compoundValue:        // compoundValue
        case symbol_kind::S_valueArray:           // valueArray
        case symbol_kind::S_valueObject:          // valueObject
        case symbol_kind::S_valueFields:          // valueFields
        case symbol_kind::S_variable:             // variable
        case symbol_kind::S_pipeline:             // pipeline
        case symbol_kind::S_stageList:            // stageList
        case symbol_kind::S_stage:                // stage
        case symbol_kind::S_inhibitOptimization:  // inhibitOptimization
        case symbol_kind::S_unionWith:            // unionWith
        case symbol_kind::S_skip:                 // skip
        case symbol_kind::S_limit:                // limit
        case symbol_kind::S_project:              // project
        case symbol_kind::S_sample:               // sample
        case symbol_kind::S_projectFields:        // projectFields
        case symbol_kind::S_projection:           // projection
        case symbol_kind::S_num:                  // num
        case symbol_kind::S_expression:           // expression
        case symbol_kind::S_compoundExpression:   // compoundExpression
        case symbol_kind::S_exprFixedTwoArg:      // exprFixedTwoArg
        case symbol_kind::S_expressionArray:      // expressionArray
        case symbol_kind::S_expressionObject:     // expressionObject
        case symbol_kind::S_expressionFields:     // expressionFields
        case symbol_kind::S_maths:                // maths
        case symbol_kind::S_add:                  // add
        case symbol_kind::S_atan2:                // atan2
        case symbol_kind::S_boolExps:             // boolExps
        case symbol_kind::S_and:                  // and
        case symbol_kind::S_or:                   // or
        case symbol_kind::S_not:                  // not
        case symbol_kind::S_literalEscapes:       // literalEscapes
        case symbol_kind::S_const:                // const
        case symbol_kind::S_literal:              // literal
        case symbol_kind::S_stringExps:           // stringExps
        case symbol_kind::S_concat:               // concat
        case symbol_kind::S_dateFromString:       // dateFromString
        case symbol_kind::S_dateToString:         // dateToString
        case symbol_kind::S_indexOfBytes:         // indexOfBytes
        case symbol_kind::S_indexOfCP:            // indexOfCP
        case symbol_kind::S_ltrim:                // ltrim
        case symbol_kind::S_regexFind:            // regexFind
        case symbol_kind::S_regexFindAll:         // regexFindAll
        case symbol_kind::S_regexMatch:           // regexMatch
        case symbol_kind::S_regexArgs:            // regexArgs
        case symbol_kind::S_replaceOne:           // replaceOne
        case symbol_kind::S_replaceAll:           // replaceAll
        case symbol_kind::S_rtrim:                // rtrim
        case symbol_kind::S_split:                // split
        case symbol_kind::S_strLenBytes:          // strLenBytes
        case symbol_kind::S_strLenCP:             // strLenCP
        case symbol_kind::S_strcasecmp:           // strcasecmp
        case symbol_kind::S_substr:               // substr
        case symbol_kind::S_substrBytes:          // substrBytes
        case symbol_kind::S_substrCP:             // substrCP
        case symbol_kind::S_toLower:              // toLower
        case symbol_kind::S_toUpper:              // toUpper
        case symbol_kind::S_trim:                 // trim
        case symbol_kind::S_compExprs:            // compExprs
        case symbol_kind::S_cmp:                  // cmp
        case symbol_kind::S_eq:                   // eq
        case symbol_kind::S_gt:                   // gt
        case symbol_kind::S_gte:                  // gte
        case symbol_kind::S_lt:                   // lt
        case symbol_kind::S_lte:                  // lte
        case symbol_kind::S_ne:                   // ne
        case symbol_kind::S_typeExpression:       // typeExpression
        case symbol_kind::S_convert:              // convert
        case symbol_kind::S_toBool:               // toBool
        case symbol_kind::S_toDate:               // toDate
        case symbol_kind::S_toDecimal:            // toDecimal
        case symbol_kind::S_toDouble:             // toDouble
        case symbol_kind::S_toInt:                // toInt
        case symbol_kind::S_toLong:               // toLong
        case symbol_kind::S_toObjectId:           // toObjectId
        case symbol_kind::S_toString:             // toString
        case symbol_kind::S_type:                 // type
        case symbol_kind::S_abs:                  // abs
        case symbol_kind::S_ceil:                 // ceil
        case symbol_kind::S_divide:               // divide
        case symbol_kind::S_exponent:             // exponent
        case symbol_kind::S_floor:                // floor
        case symbol_kind::S_ln:                   // ln
        case symbol_kind::S_log:                  // log
        case symbol_kind::S_logten:               // logten
        case symbol_kind::S_mod:                  // mod
        case symbol_kind::S_multiply:             // multiply
        case symbol_kind::S_pow:                  // pow
        case symbol_kind::S_round:                // round
        case symbol_kind::S_sqrt:                 // sqrt
        case symbol_kind::S_subtract:             // subtract
        case symbol_kind::S_trunc:                // trunc
        case symbol_kind::S_setExpression:        // setExpression
        case symbol_kind::S_allElementsTrue:      // allElementsTrue
        case symbol_kind::S_anyElementTrue:       // anyElementTrue
        case symbol_kind::S_setDifference:        // setDifference
        case symbol_kind::S_setEquals:            // setEquals
        case symbol_kind::S_setIntersection:      // setIntersection
        case symbol_kind::S_setIsSubset:          // setIsSubset
        case symbol_kind::S_setUnion:             // setUnion
        case symbol_kind::S_match:                // match
        case symbol_kind::S_predicates:           // predicates
        case symbol_kind::S_compoundMatchExprs:   // compoundMatchExprs
        case symbol_kind::S_predValue:            // predValue
        case symbol_kind::S_additionalExprs:      // additionalExprs
        case symbol_kind::S_sortSpecs:            // sortSpecs
        case symbol_kind::S_specList:             // specList
        case symbol_kind::S_metaSort:             // metaSort
        case symbol_kind::S_oneOrNegOne:          // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:      // metaSortKeyword
            value.move<CNode>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_projectionFieldname:      // projectionFieldname
        case symbol_kind::S_expressionFieldname:      // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
        case symbol_kind::S_predFieldname:            // predFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
        case symbol_kind::S_logicalExprField:         // logicalExprField
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

        case symbol_kind::S_projectField:        // projectField
        case symbol_kind::S_expressionField:     // expressionField
        case symbol_kind::S_valueField:          // valueField
        case symbol_kind::S_onErrorArg:          // onErrorArg
        case symbol_kind::S_onNullArg:           // onNullArg
        case symbol_kind::S_formatArg:           // formatArg
        case symbol_kind::S_timezoneArg:         // timezoneArg
        case symbol_kind::S_charsArg:            // charsArg
        case symbol_kind::S_optionsArg:          // optionsArg
        case symbol_kind::S_predicate:           // predicate
        case symbol_kind::S_logicalExpr:         // logicalExpr
        case symbol_kind::S_operatorExpression:  // operatorExpression
        case symbol_kind::S_notExpr:             // notExpr
        case symbol_kind::S_sortSpec:            // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
            value.move<std::string>(YY_MOVE(that.value));
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
            value.move<std::vector<CNode>>(YY_MOVE(that.value));
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

        case symbol_kind::S_dbPointer:            // dbPointer
        case symbol_kind::S_javascript:           // javascript
        case symbol_kind::S_symbol:               // symbol
        case symbol_kind::S_javascriptWScope:     // javascriptWScope
        case symbol_kind::S_int:                  // int
        case symbol_kind::S_timestamp:            // timestamp
        case symbol_kind::S_long:                 // long
        case symbol_kind::S_double:               // double
        case symbol_kind::S_decimal:              // decimal
        case symbol_kind::S_minKey:               // minKey
        case symbol_kind::S_maxKey:               // maxKey
        case symbol_kind::S_value:                // value
        case symbol_kind::S_string:               // string
        case symbol_kind::S_fieldPath:            // fieldPath
        case symbol_kind::S_binary:               // binary
        case symbol_kind::S_undefined:            // undefined
        case symbol_kind::S_objectId:             // objectId
        case symbol_kind::S_bool:                 // bool
        case symbol_kind::S_date:                 // date
        case symbol_kind::S_null:                 // null
        case symbol_kind::S_regex:                // regex
        case symbol_kind::S_simpleValue:          // simpleValue
        case symbol_kind::S_compoundValue:        // compoundValue
        case symbol_kind::S_valueArray:           // valueArray
        case symbol_kind::S_valueObject:          // valueObject
        case symbol_kind::S_valueFields:          // valueFields
        case symbol_kind::S_variable:             // variable
        case symbol_kind::S_pipeline:             // pipeline
        case symbol_kind::S_stageList:            // stageList
        case symbol_kind::S_stage:                // stage
        case symbol_kind::S_inhibitOptimization:  // inhibitOptimization
        case symbol_kind::S_unionWith:            // unionWith
        case symbol_kind::S_skip:                 // skip
        case symbol_kind::S_limit:                // limit
        case symbol_kind::S_project:              // project
        case symbol_kind::S_sample:               // sample
        case symbol_kind::S_projectFields:        // projectFields
        case symbol_kind::S_projection:           // projection
        case symbol_kind::S_num:                  // num
        case symbol_kind::S_expression:           // expression
        case symbol_kind::S_compoundExpression:   // compoundExpression
        case symbol_kind::S_exprFixedTwoArg:      // exprFixedTwoArg
        case symbol_kind::S_expressionArray:      // expressionArray
        case symbol_kind::S_expressionObject:     // expressionObject
        case symbol_kind::S_expressionFields:     // expressionFields
        case symbol_kind::S_maths:                // maths
        case symbol_kind::S_add:                  // add
        case symbol_kind::S_atan2:                // atan2
        case symbol_kind::S_boolExps:             // boolExps
        case symbol_kind::S_and:                  // and
        case symbol_kind::S_or:                   // or
        case symbol_kind::S_not:                  // not
        case symbol_kind::S_literalEscapes:       // literalEscapes
        case symbol_kind::S_const:                // const
        case symbol_kind::S_literal:              // literal
        case symbol_kind::S_stringExps:           // stringExps
        case symbol_kind::S_concat:               // concat
        case symbol_kind::S_dateFromString:       // dateFromString
        case symbol_kind::S_dateToString:         // dateToString
        case symbol_kind::S_indexOfBytes:         // indexOfBytes
        case symbol_kind::S_indexOfCP:            // indexOfCP
        case symbol_kind::S_ltrim:                // ltrim
        case symbol_kind::S_regexFind:            // regexFind
        case symbol_kind::S_regexFindAll:         // regexFindAll
        case symbol_kind::S_regexMatch:           // regexMatch
        case symbol_kind::S_regexArgs:            // regexArgs
        case symbol_kind::S_replaceOne:           // replaceOne
        case symbol_kind::S_replaceAll:           // replaceAll
        case symbol_kind::S_rtrim:                // rtrim
        case symbol_kind::S_split:                // split
        case symbol_kind::S_strLenBytes:          // strLenBytes
        case symbol_kind::S_strLenCP:             // strLenCP
        case symbol_kind::S_strcasecmp:           // strcasecmp
        case symbol_kind::S_substr:               // substr
        case symbol_kind::S_substrBytes:          // substrBytes
        case symbol_kind::S_substrCP:             // substrCP
        case symbol_kind::S_toLower:              // toLower
        case symbol_kind::S_toUpper:              // toUpper
        case symbol_kind::S_trim:                 // trim
        case symbol_kind::S_compExprs:            // compExprs
        case symbol_kind::S_cmp:                  // cmp
        case symbol_kind::S_eq:                   // eq
        case symbol_kind::S_gt:                   // gt
        case symbol_kind::S_gte:                  // gte
        case symbol_kind::S_lt:                   // lt
        case symbol_kind::S_lte:                  // lte
        case symbol_kind::S_ne:                   // ne
        case symbol_kind::S_typeExpression:       // typeExpression
        case symbol_kind::S_convert:              // convert
        case symbol_kind::S_toBool:               // toBool
        case symbol_kind::S_toDate:               // toDate
        case symbol_kind::S_toDecimal:            // toDecimal
        case symbol_kind::S_toDouble:             // toDouble
        case symbol_kind::S_toInt:                // toInt
        case symbol_kind::S_toLong:               // toLong
        case symbol_kind::S_toObjectId:           // toObjectId
        case symbol_kind::S_toString:             // toString
        case symbol_kind::S_type:                 // type
        case symbol_kind::S_abs:                  // abs
        case symbol_kind::S_ceil:                 // ceil
        case symbol_kind::S_divide:               // divide
        case symbol_kind::S_exponent:             // exponent
        case symbol_kind::S_floor:                // floor
        case symbol_kind::S_ln:                   // ln
        case symbol_kind::S_log:                  // log
        case symbol_kind::S_logten:               // logten
        case symbol_kind::S_mod:                  // mod
        case symbol_kind::S_multiply:             // multiply
        case symbol_kind::S_pow:                  // pow
        case symbol_kind::S_round:                // round
        case symbol_kind::S_sqrt:                 // sqrt
        case symbol_kind::S_subtract:             // subtract
        case symbol_kind::S_trunc:                // trunc
        case symbol_kind::S_setExpression:        // setExpression
        case symbol_kind::S_allElementsTrue:      // allElementsTrue
        case symbol_kind::S_anyElementTrue:       // anyElementTrue
        case symbol_kind::S_setDifference:        // setDifference
        case symbol_kind::S_setEquals:            // setEquals
        case symbol_kind::S_setIntersection:      // setIntersection
        case symbol_kind::S_setIsSubset:          // setIsSubset
        case symbol_kind::S_setUnion:             // setUnion
        case symbol_kind::S_match:                // match
        case symbol_kind::S_predicates:           // predicates
        case symbol_kind::S_compoundMatchExprs:   // compoundMatchExprs
        case symbol_kind::S_predValue:            // predValue
        case symbol_kind::S_additionalExprs:      // additionalExprs
        case symbol_kind::S_sortSpecs:            // sortSpecs
        case symbol_kind::S_specList:             // specList
        case symbol_kind::S_metaSort:             // metaSort
        case symbol_kind::S_oneOrNegOne:          // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:      // metaSortKeyword
            value.copy<CNode>(that.value);
            break;

        case symbol_kind::S_projectionFieldname:      // projectionFieldname
        case symbol_kind::S_expressionFieldname:      // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
        case symbol_kind::S_predFieldname:            // predFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
        case symbol_kind::S_logicalExprField:         // logicalExprField
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

        case symbol_kind::S_projectField:        // projectField
        case symbol_kind::S_expressionField:     // expressionField
        case symbol_kind::S_valueField:          // valueField
        case symbol_kind::S_onErrorArg:          // onErrorArg
        case symbol_kind::S_onNullArg:           // onNullArg
        case symbol_kind::S_formatArg:           // formatArg
        case symbol_kind::S_timezoneArg:         // timezoneArg
        case symbol_kind::S_charsArg:            // charsArg
        case symbol_kind::S_optionsArg:          // optionsArg
        case symbol_kind::S_predicate:           // predicate
        case symbol_kind::S_logicalExpr:         // logicalExpr
        case symbol_kind::S_operatorExpression:  // operatorExpression
        case symbol_kind::S_notExpr:             // notExpr
        case symbol_kind::S_sortSpec:            // sortSpec
            value.copy<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
            value.copy<std::string>(that.value);
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
            value.copy<std::vector<CNode>>(that.value);
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

        case symbol_kind::S_dbPointer:            // dbPointer
        case symbol_kind::S_javascript:           // javascript
        case symbol_kind::S_symbol:               // symbol
        case symbol_kind::S_javascriptWScope:     // javascriptWScope
        case symbol_kind::S_int:                  // int
        case symbol_kind::S_timestamp:            // timestamp
        case symbol_kind::S_long:                 // long
        case symbol_kind::S_double:               // double
        case symbol_kind::S_decimal:              // decimal
        case symbol_kind::S_minKey:               // minKey
        case symbol_kind::S_maxKey:               // maxKey
        case symbol_kind::S_value:                // value
        case symbol_kind::S_string:               // string
        case symbol_kind::S_fieldPath:            // fieldPath
        case symbol_kind::S_binary:               // binary
        case symbol_kind::S_undefined:            // undefined
        case symbol_kind::S_objectId:             // objectId
        case symbol_kind::S_bool:                 // bool
        case symbol_kind::S_date:                 // date
        case symbol_kind::S_null:                 // null
        case symbol_kind::S_regex:                // regex
        case symbol_kind::S_simpleValue:          // simpleValue
        case symbol_kind::S_compoundValue:        // compoundValue
        case symbol_kind::S_valueArray:           // valueArray
        case symbol_kind::S_valueObject:          // valueObject
        case symbol_kind::S_valueFields:          // valueFields
        case symbol_kind::S_variable:             // variable
        case symbol_kind::S_pipeline:             // pipeline
        case symbol_kind::S_stageList:            // stageList
        case symbol_kind::S_stage:                // stage
        case symbol_kind::S_inhibitOptimization:  // inhibitOptimization
        case symbol_kind::S_unionWith:            // unionWith
        case symbol_kind::S_skip:                 // skip
        case symbol_kind::S_limit:                // limit
        case symbol_kind::S_project:              // project
        case symbol_kind::S_sample:               // sample
        case symbol_kind::S_projectFields:        // projectFields
        case symbol_kind::S_projection:           // projection
        case symbol_kind::S_num:                  // num
        case symbol_kind::S_expression:           // expression
        case symbol_kind::S_compoundExpression:   // compoundExpression
        case symbol_kind::S_exprFixedTwoArg:      // exprFixedTwoArg
        case symbol_kind::S_expressionArray:      // expressionArray
        case symbol_kind::S_expressionObject:     // expressionObject
        case symbol_kind::S_expressionFields:     // expressionFields
        case symbol_kind::S_maths:                // maths
        case symbol_kind::S_add:                  // add
        case symbol_kind::S_atan2:                // atan2
        case symbol_kind::S_boolExps:             // boolExps
        case symbol_kind::S_and:                  // and
        case symbol_kind::S_or:                   // or
        case symbol_kind::S_not:                  // not
        case symbol_kind::S_literalEscapes:       // literalEscapes
        case symbol_kind::S_const:                // const
        case symbol_kind::S_literal:              // literal
        case symbol_kind::S_stringExps:           // stringExps
        case symbol_kind::S_concat:               // concat
        case symbol_kind::S_dateFromString:       // dateFromString
        case symbol_kind::S_dateToString:         // dateToString
        case symbol_kind::S_indexOfBytes:         // indexOfBytes
        case symbol_kind::S_indexOfCP:            // indexOfCP
        case symbol_kind::S_ltrim:                // ltrim
        case symbol_kind::S_regexFind:            // regexFind
        case symbol_kind::S_regexFindAll:         // regexFindAll
        case symbol_kind::S_regexMatch:           // regexMatch
        case symbol_kind::S_regexArgs:            // regexArgs
        case symbol_kind::S_replaceOne:           // replaceOne
        case symbol_kind::S_replaceAll:           // replaceAll
        case symbol_kind::S_rtrim:                // rtrim
        case symbol_kind::S_split:                // split
        case symbol_kind::S_strLenBytes:          // strLenBytes
        case symbol_kind::S_strLenCP:             // strLenCP
        case symbol_kind::S_strcasecmp:           // strcasecmp
        case symbol_kind::S_substr:               // substr
        case symbol_kind::S_substrBytes:          // substrBytes
        case symbol_kind::S_substrCP:             // substrCP
        case symbol_kind::S_toLower:              // toLower
        case symbol_kind::S_toUpper:              // toUpper
        case symbol_kind::S_trim:                 // trim
        case symbol_kind::S_compExprs:            // compExprs
        case symbol_kind::S_cmp:                  // cmp
        case symbol_kind::S_eq:                   // eq
        case symbol_kind::S_gt:                   // gt
        case symbol_kind::S_gte:                  // gte
        case symbol_kind::S_lt:                   // lt
        case symbol_kind::S_lte:                  // lte
        case symbol_kind::S_ne:                   // ne
        case symbol_kind::S_typeExpression:       // typeExpression
        case symbol_kind::S_convert:              // convert
        case symbol_kind::S_toBool:               // toBool
        case symbol_kind::S_toDate:               // toDate
        case symbol_kind::S_toDecimal:            // toDecimal
        case symbol_kind::S_toDouble:             // toDouble
        case symbol_kind::S_toInt:                // toInt
        case symbol_kind::S_toLong:               // toLong
        case symbol_kind::S_toObjectId:           // toObjectId
        case symbol_kind::S_toString:             // toString
        case symbol_kind::S_type:                 // type
        case symbol_kind::S_abs:                  // abs
        case symbol_kind::S_ceil:                 // ceil
        case symbol_kind::S_divide:               // divide
        case symbol_kind::S_exponent:             // exponent
        case symbol_kind::S_floor:                // floor
        case symbol_kind::S_ln:                   // ln
        case symbol_kind::S_log:                  // log
        case symbol_kind::S_logten:               // logten
        case symbol_kind::S_mod:                  // mod
        case symbol_kind::S_multiply:             // multiply
        case symbol_kind::S_pow:                  // pow
        case symbol_kind::S_round:                // round
        case symbol_kind::S_sqrt:                 // sqrt
        case symbol_kind::S_subtract:             // subtract
        case symbol_kind::S_trunc:                // trunc
        case symbol_kind::S_setExpression:        // setExpression
        case symbol_kind::S_allElementsTrue:      // allElementsTrue
        case symbol_kind::S_anyElementTrue:       // anyElementTrue
        case symbol_kind::S_setDifference:        // setDifference
        case symbol_kind::S_setEquals:            // setEquals
        case symbol_kind::S_setIntersection:      // setIntersection
        case symbol_kind::S_setIsSubset:          // setIsSubset
        case symbol_kind::S_setUnion:             // setUnion
        case symbol_kind::S_match:                // match
        case symbol_kind::S_predicates:           // predicates
        case symbol_kind::S_compoundMatchExprs:   // compoundMatchExprs
        case symbol_kind::S_predValue:            // predValue
        case symbol_kind::S_additionalExprs:      // additionalExprs
        case symbol_kind::S_sortSpecs:            // sortSpecs
        case symbol_kind::S_specList:             // specList
        case symbol_kind::S_metaSort:             // metaSort
        case symbol_kind::S_oneOrNegOne:          // oneOrNegOne
        case symbol_kind::S_metaSortKeyword:      // metaSortKeyword
            value.move<CNode>(that.value);
            break;

        case symbol_kind::S_projectionFieldname:      // projectionFieldname
        case symbol_kind::S_expressionFieldname:      // expressionFieldname
        case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
        case symbol_kind::S_predFieldname:            // predFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
        case symbol_kind::S_logicalExprField:         // logicalExprField
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

        case symbol_kind::S_projectField:        // projectField
        case symbol_kind::S_expressionField:     // expressionField
        case symbol_kind::S_valueField:          // valueField
        case symbol_kind::S_onErrorArg:          // onErrorArg
        case symbol_kind::S_onNullArg:           // onNullArg
        case symbol_kind::S_formatArg:           // formatArg
        case symbol_kind::S_timezoneArg:         // timezoneArg
        case symbol_kind::S_charsArg:            // charsArg
        case symbol_kind::S_optionsArg:          // optionsArg
        case symbol_kind::S_predicate:           // predicate
        case symbol_kind::S_logicalExpr:         // logicalExpr
        case symbol_kind::S_operatorExpression:  // operatorExpression
        case symbol_kind::S_notExpr:             // notExpr
        case symbol_kind::S_sortSpec:            // sortSpec
            value.move<std::pair<CNode::Fieldname, CNode>>(that.value);
            break;

        case symbol_kind::S_FIELDNAME:              // "fieldname"
        case symbol_kind::S_STRING:                 // "string"
        case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
        case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
        case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
            value.move<std::string>(that.value);
            break;

        case symbol_kind::S_expressions:    // expressions
        case symbol_kind::S_values:         // values
        case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
            value.move<std::vector<CNode>>(that.value);
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

                case symbol_kind::S_dbPointer:            // dbPointer
                case symbol_kind::S_javascript:           // javascript
                case symbol_kind::S_symbol:               // symbol
                case symbol_kind::S_javascriptWScope:     // javascriptWScope
                case symbol_kind::S_int:                  // int
                case symbol_kind::S_timestamp:            // timestamp
                case symbol_kind::S_long:                 // long
                case symbol_kind::S_double:               // double
                case symbol_kind::S_decimal:              // decimal
                case symbol_kind::S_minKey:               // minKey
                case symbol_kind::S_maxKey:               // maxKey
                case symbol_kind::S_value:                // value
                case symbol_kind::S_string:               // string
                case symbol_kind::S_fieldPath:            // fieldPath
                case symbol_kind::S_binary:               // binary
                case symbol_kind::S_undefined:            // undefined
                case symbol_kind::S_objectId:             // objectId
                case symbol_kind::S_bool:                 // bool
                case symbol_kind::S_date:                 // date
                case symbol_kind::S_null:                 // null
                case symbol_kind::S_regex:                // regex
                case symbol_kind::S_simpleValue:          // simpleValue
                case symbol_kind::S_compoundValue:        // compoundValue
                case symbol_kind::S_valueArray:           // valueArray
                case symbol_kind::S_valueObject:          // valueObject
                case symbol_kind::S_valueFields:          // valueFields
                case symbol_kind::S_variable:             // variable
                case symbol_kind::S_pipeline:             // pipeline
                case symbol_kind::S_stageList:            // stageList
                case symbol_kind::S_stage:                // stage
                case symbol_kind::S_inhibitOptimization:  // inhibitOptimization
                case symbol_kind::S_unionWith:            // unionWith
                case symbol_kind::S_skip:                 // skip
                case symbol_kind::S_limit:                // limit
                case symbol_kind::S_project:              // project
                case symbol_kind::S_sample:               // sample
                case symbol_kind::S_projectFields:        // projectFields
                case symbol_kind::S_projection:           // projection
                case symbol_kind::S_num:                  // num
                case symbol_kind::S_expression:           // expression
                case symbol_kind::S_compoundExpression:   // compoundExpression
                case symbol_kind::S_exprFixedTwoArg:      // exprFixedTwoArg
                case symbol_kind::S_expressionArray:      // expressionArray
                case symbol_kind::S_expressionObject:     // expressionObject
                case symbol_kind::S_expressionFields:     // expressionFields
                case symbol_kind::S_maths:                // maths
                case symbol_kind::S_add:                  // add
                case symbol_kind::S_atan2:                // atan2
                case symbol_kind::S_boolExps:             // boolExps
                case symbol_kind::S_and:                  // and
                case symbol_kind::S_or:                   // or
                case symbol_kind::S_not:                  // not
                case symbol_kind::S_literalEscapes:       // literalEscapes
                case symbol_kind::S_const:                // const
                case symbol_kind::S_literal:              // literal
                case symbol_kind::S_stringExps:           // stringExps
                case symbol_kind::S_concat:               // concat
                case symbol_kind::S_dateFromString:       // dateFromString
                case symbol_kind::S_dateToString:         // dateToString
                case symbol_kind::S_indexOfBytes:         // indexOfBytes
                case symbol_kind::S_indexOfCP:            // indexOfCP
                case symbol_kind::S_ltrim:                // ltrim
                case symbol_kind::S_regexFind:            // regexFind
                case symbol_kind::S_regexFindAll:         // regexFindAll
                case symbol_kind::S_regexMatch:           // regexMatch
                case symbol_kind::S_regexArgs:            // regexArgs
                case symbol_kind::S_replaceOne:           // replaceOne
                case symbol_kind::S_replaceAll:           // replaceAll
                case symbol_kind::S_rtrim:                // rtrim
                case symbol_kind::S_split:                // split
                case symbol_kind::S_strLenBytes:          // strLenBytes
                case symbol_kind::S_strLenCP:             // strLenCP
                case symbol_kind::S_strcasecmp:           // strcasecmp
                case symbol_kind::S_substr:               // substr
                case symbol_kind::S_substrBytes:          // substrBytes
                case symbol_kind::S_substrCP:             // substrCP
                case symbol_kind::S_toLower:              // toLower
                case symbol_kind::S_toUpper:              // toUpper
                case symbol_kind::S_trim:                 // trim
                case symbol_kind::S_compExprs:            // compExprs
                case symbol_kind::S_cmp:                  // cmp
                case symbol_kind::S_eq:                   // eq
                case symbol_kind::S_gt:                   // gt
                case symbol_kind::S_gte:                  // gte
                case symbol_kind::S_lt:                   // lt
                case symbol_kind::S_lte:                  // lte
                case symbol_kind::S_ne:                   // ne
                case symbol_kind::S_typeExpression:       // typeExpression
                case symbol_kind::S_convert:              // convert
                case symbol_kind::S_toBool:               // toBool
                case symbol_kind::S_toDate:               // toDate
                case symbol_kind::S_toDecimal:            // toDecimal
                case symbol_kind::S_toDouble:             // toDouble
                case symbol_kind::S_toInt:                // toInt
                case symbol_kind::S_toLong:               // toLong
                case symbol_kind::S_toObjectId:           // toObjectId
                case symbol_kind::S_toString:             // toString
                case symbol_kind::S_type:                 // type
                case symbol_kind::S_abs:                  // abs
                case symbol_kind::S_ceil:                 // ceil
                case symbol_kind::S_divide:               // divide
                case symbol_kind::S_exponent:             // exponent
                case symbol_kind::S_floor:                // floor
                case symbol_kind::S_ln:                   // ln
                case symbol_kind::S_log:                  // log
                case symbol_kind::S_logten:               // logten
                case symbol_kind::S_mod:                  // mod
                case symbol_kind::S_multiply:             // multiply
                case symbol_kind::S_pow:                  // pow
                case symbol_kind::S_round:                // round
                case symbol_kind::S_sqrt:                 // sqrt
                case symbol_kind::S_subtract:             // subtract
                case symbol_kind::S_trunc:                // trunc
                case symbol_kind::S_setExpression:        // setExpression
                case symbol_kind::S_allElementsTrue:      // allElementsTrue
                case symbol_kind::S_anyElementTrue:       // anyElementTrue
                case symbol_kind::S_setDifference:        // setDifference
                case symbol_kind::S_setEquals:            // setEquals
                case symbol_kind::S_setIntersection:      // setIntersection
                case symbol_kind::S_setIsSubset:          // setIsSubset
                case symbol_kind::S_setUnion:             // setUnion
                case symbol_kind::S_match:                // match
                case symbol_kind::S_predicates:           // predicates
                case symbol_kind::S_compoundMatchExprs:   // compoundMatchExprs
                case symbol_kind::S_predValue:            // predValue
                case symbol_kind::S_additionalExprs:      // additionalExprs
                case symbol_kind::S_sortSpecs:            // sortSpecs
                case symbol_kind::S_specList:             // specList
                case symbol_kind::S_metaSort:             // metaSort
                case symbol_kind::S_oneOrNegOne:          // oneOrNegOne
                case symbol_kind::S_metaSortKeyword:      // metaSortKeyword
                    yylhs.value.emplace<CNode>();
                    break;

                case symbol_kind::S_projectionFieldname:      // projectionFieldname
                case symbol_kind::S_expressionFieldname:      // expressionFieldname
                case symbol_kind::S_stageAsUserFieldname:     // stageAsUserFieldname
                case symbol_kind::S_predFieldname:            // predFieldname
                case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
                case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
                case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
                case symbol_kind::S_valueFieldname:           // valueFieldname
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

                case symbol_kind::S_projectField:        // projectField
                case symbol_kind::S_expressionField:     // expressionField
                case symbol_kind::S_valueField:          // valueField
                case symbol_kind::S_onErrorArg:          // onErrorArg
                case symbol_kind::S_onNullArg:           // onNullArg
                case symbol_kind::S_formatArg:           // formatArg
                case symbol_kind::S_timezoneArg:         // timezoneArg
                case symbol_kind::S_charsArg:            // charsArg
                case symbol_kind::S_optionsArg:          // optionsArg
                case symbol_kind::S_predicate:           // predicate
                case symbol_kind::S_logicalExpr:         // logicalExpr
                case symbol_kind::S_operatorExpression:  // operatorExpression
                case symbol_kind::S_notExpr:             // notExpr
                case symbol_kind::S_sortSpec:            // sortSpec
                    yylhs.value.emplace<std::pair<CNode::Fieldname, CNode>>();
                    break;

                case symbol_kind::S_FIELDNAME:              // "fieldname"
                case symbol_kind::S_STRING:                 // "string"
                case symbol_kind::S_DOLLAR_STRING:          // "$-prefixed string"
                case symbol_kind::S_DOLLAR_DOLLAR_STRING:   // "$$-prefixed string"
                case symbol_kind::S_DOLLAR_PREF_FIELDNAME:  // "$-prefixed fieldname"
                    yylhs.value.emplace<std::string>();
                    break;

                case symbol_kind::S_expressions:    // expressions
                case symbol_kind::S_values:         // values
                case symbol_kind::S_exprZeroToTwo:  // exprZeroToTwo
                    yylhs.value.emplace<std::vector<CNode>>();
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
                    case 2:  // start: "pipeline argument" pipeline
#line 313 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1788 "parser_gen.cpp"
                    break;

                    case 3:  // start: "filter" match
#line 316 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1796 "parser_gen.cpp"
                    break;

                    case 4:  // start: "query" match
#line 319 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1804 "parser_gen.cpp"
                    break;

                    case 5:  // start: "q" match
#line 322 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1812 "parser_gen.cpp"
                    break;

                    case 6:  // start: "sort argument" sortSpecs
#line 325 "grammar.yy"
                    {
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1820 "parser_gen.cpp"
                    break;

                    case 7:  // pipeline: "array" stageList "end of array"
#line 332 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1828 "parser_gen.cpp"
                    break;

                    case 8:  // stageList: %empty
#line 338 "grammar.yy"
                    {
                    }
#line 1834 "parser_gen.cpp"
                    break;

                    case 9:  // stageList: "object" stage "end of object" stageList
#line 339 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1842 "parser_gen.cpp"
                    break;

                    case 10:  // $@1: %empty
#line 347 "grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1848 "parser_gen.cpp"
                    break;

                    case 12:  // stage: inhibitOptimization
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1854 "parser_gen.cpp"
                    break;

                    case 13:  // stage: unionWith
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1860 "parser_gen.cpp"
                    break;

                    case 14:  // stage: skip
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1866 "parser_gen.cpp"
                    break;

                    case 15:  // stage: limit
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1872 "parser_gen.cpp"
                    break;

                    case 16:  // stage: project
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1878 "parser_gen.cpp"
                    break;

                    case 17:  // stage: sample
#line 350 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1884 "parser_gen.cpp"
                    break;

                    case 18:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 353 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1896 "parser_gen.cpp"
                    break;

                    case 19:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 363 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1904 "parser_gen.cpp"
                    break;

                    case 20:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 369 "grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1917 "parser_gen.cpp"
                    break;

                    case 21:  // num: int
#line 379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1923 "parser_gen.cpp"
                    break;

                    case 22:  // num: long
#line 379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1929 "parser_gen.cpp"
                    break;

                    case 23:  // num: double
#line 379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1935 "parser_gen.cpp"
                    break;

                    case 24:  // num: decimal
#line 379 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1941 "parser_gen.cpp"
                    break;

                    case 25:  // skip: STAGE_SKIP num
#line 383 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1949 "parser_gen.cpp"
                    break;

                    case 26:  // limit: STAGE_LIMIT num
#line 388 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1957 "parser_gen.cpp"
                    break;

                    case 27:  // project: STAGE_PROJECT "object" projectFields "end of object"
#line 393 "grammar.yy"
                    {
                        auto&& fields = YY_MOVE(yystack_[1].value.as<CNode>());
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
#line 1975 "parser_gen.cpp"
                    break;

                    case 28:  // projectFields: %empty
#line 409 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1983 "parser_gen.cpp"
                    break;

                    case 29:  // projectFields: projectFields projectField
#line 412 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1992 "parser_gen.cpp"
                    break;

                    case 30:  // projectField: ID projection
#line 419 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2000 "parser_gen.cpp"
                    break;

                    case 31:  // projectField: projectionFieldname projection
#line 422 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2008 "parser_gen.cpp"
                    break;

                    case 32:  // projection: string
#line 428 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2014 "parser_gen.cpp"
                    break;

                    case 33:  // projection: binary
#line 429 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2020 "parser_gen.cpp"
                    break;

                    case 34:  // projection: undefined
#line 430 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2026 "parser_gen.cpp"
                    break;

                    case 35:  // projection: objectId
#line 431 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2032 "parser_gen.cpp"
                    break;

                    case 36:  // projection: date
#line 432 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2038 "parser_gen.cpp"
                    break;

                    case 37:  // projection: null
#line 433 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2044 "parser_gen.cpp"
                    break;

                    case 38:  // projection: regex
#line 434 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2050 "parser_gen.cpp"
                    break;

                    case 39:  // projection: dbPointer
#line 435 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2056 "parser_gen.cpp"
                    break;

                    case 40:  // projection: javascript
#line 436 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2062 "parser_gen.cpp"
                    break;

                    case 41:  // projection: symbol
#line 437 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2068 "parser_gen.cpp"
                    break;

                    case 42:  // projection: javascriptWScope
#line 438 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2074 "parser_gen.cpp"
                    break;

                    case 43:  // projection: "1 (int)"
#line 439 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 2082 "parser_gen.cpp"
                    break;

                    case 44:  // projection: "-1 (int)"
#line 442 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2090 "parser_gen.cpp"
                    break;

                    case 45:  // projection: "arbitrary integer"
#line 445 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2098 "parser_gen.cpp"
                    break;

                    case 46:  // projection: "zero (int)"
#line 448 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2106 "parser_gen.cpp"
                    break;

                    case 47:  // projection: "1 (long)"
#line 451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2114 "parser_gen.cpp"
                    break;

                    case 48:  // projection: "-1 (long)"
#line 454 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2122 "parser_gen.cpp"
                    break;

                    case 49:  // projection: "arbitrary long"
#line 457 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2130 "parser_gen.cpp"
                    break;

                    case 50:  // projection: "zero (long)"
#line 460 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2138 "parser_gen.cpp"
                    break;

                    case 51:  // projection: "1 (double)"
#line 463 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2146 "parser_gen.cpp"
                    break;

                    case 52:  // projection: "-1 (double)"
#line 466 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2154 "parser_gen.cpp"
                    break;

                    case 53:  // projection: "arbitrary double"
#line 469 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2162 "parser_gen.cpp"
                    break;

                    case 54:  // projection: "zero (double)"
#line 472 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2170 "parser_gen.cpp"
                    break;

                    case 55:  // projection: "1 (decimal)"
#line 475 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2178 "parser_gen.cpp"
                    break;

                    case 56:  // projection: "-1 (decimal)"
#line 478 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2186 "parser_gen.cpp"
                    break;

                    case 57:  // projection: "arbitrary decimal"
#line 481 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2194 "parser_gen.cpp"
                    break;

                    case 58:  // projection: "zero (decimal)"
#line 484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2202 "parser_gen.cpp"
                    break;

                    case 59:  // projection: "true"
#line 487 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2210 "parser_gen.cpp"
                    break;

                    case 60:  // projection: "false"
#line 490 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2218 "parser_gen.cpp"
                    break;

                    case 61:  // projection: timestamp
#line 493 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2224 "parser_gen.cpp"
                    break;

                    case 62:  // projection: minKey
#line 494 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2230 "parser_gen.cpp"
                    break;

                    case 63:  // projection: maxKey
#line 495 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2236 "parser_gen.cpp"
                    break;

                    case 64:  // projection: compoundExpression
#line 496 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            c_node_disambiguation::disambiguateCompoundProjection(
                                YY_MOVE(yystack_[0].value.as<CNode>()));
                        if (stdx::holds_alternative<CompoundInconsistentKey>(
                                yylhs.value.as<CNode>().payload))
                            // TODO SERVER-50498: error() instead of uasserting
                            uasserted(ErrorCodes::FailedToParse,
                                      "object project field cannot contain both inclusion and "
                                      "exclusion indicators");
                    }
#line 2247 "parser_gen.cpp"
                    break;

                    case 65:  // projectionFieldname: invariableUserFieldname
#line 505 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2253 "parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: stageAsUserFieldname
#line 505 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2259 "parser_gen.cpp"
                    break;

                    case 67:  // projectionFieldname: argAsUserFieldname
#line 505 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2265 "parser_gen.cpp"
                    break;

                    case 68:  // projectionFieldname: aggExprAsUserFieldname
#line 505 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2271 "parser_gen.cpp"
                    break;

                    case 69:  // match: "object" predicates "end of object"
#line 509 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2279 "parser_gen.cpp"
                    break;

                    case 70:  // predicates: %empty
#line 515 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2287 "parser_gen.cpp"
                    break;

                    case 71:  // predicates: predicates predicate
#line 518 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2296 "parser_gen.cpp"
                    break;

                    case 72:  // predicate: predFieldname predValue
#line 524 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2304 "parser_gen.cpp"
                    break;

                    case 73:  // predicate: logicalExpr
#line 527 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2312 "parser_gen.cpp"
                    break;

                    case 74:  // predValue: simpleValue
#line 536 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2318 "parser_gen.cpp"
                    break;

                    case 75:  // predValue: "object" compoundMatchExprs "end of object"
#line 537 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2326 "parser_gen.cpp"
                    break;

                    case 76:  // compoundMatchExprs: %empty
#line 543 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2334 "parser_gen.cpp"
                    break;

                    case 77:  // compoundMatchExprs: compoundMatchExprs operatorExpression
#line 546 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2343 "parser_gen.cpp"
                    break;

                    case 78:  // operatorExpression: notExpr
#line 553 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>());
                    }
#line 2349 "parser_gen.cpp"
                    break;

                    case 79:  // notExpr: NOT regex
#line 556 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::notExpr, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2357 "parser_gen.cpp"
                    break;

                    case 80:  // notExpr: NOT "object" operatorExpression compoundMatchExprs "end of
                              // object"
#line 560 "grammar.yy"
                    {
                        auto&& exprs = YY_MOVE(yystack_[1].value.as<CNode>());
                        exprs.objectChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<std::pair<CNode::Fieldname, CNode>>()));

                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::notExpr, std::move(exprs)};
                    }
#line 2368 "parser_gen.cpp"
                    break;

                    case 81:  // logicalExpr: logicalExprField "array" match additionalExprs "end of
                              // array"
#line 569 "grammar.yy"
                    {
                        auto&& children = YY_MOVE(yystack_[1].value.as<CNode>());
                        children.arrayChildren().emplace_back(
                            YY_MOVE(yystack_[2].value.as<CNode>()));
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[4].value.as<CNode::Fieldname>()), std::move(children)};
                    }
#line 2378 "parser_gen.cpp"
                    break;

                    case 82:  // logicalExprField: AND
#line 577 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::andExpr;
                    }
#line 2384 "parser_gen.cpp"
                    break;

                    case 83:  // logicalExprField: OR
#line 578 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::orExpr;
                    }
#line 2390 "parser_gen.cpp"
                    break;

                    case 84:  // logicalExprField: NOR
#line 579 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = KeyFieldname::norExpr;
                    }
#line 2396 "parser_gen.cpp"
                    break;

                    case 85:  // additionalExprs: %empty
#line 582 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ArrayChildren{}};
                    }
#line 2404 "parser_gen.cpp"
                    break;

                    case 86:  // additionalExprs: additionalExprs match
#line 585 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().arrayChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<CNode>()));
                    }
#line 2413 "parser_gen.cpp"
                    break;

                    case 87:  // predFieldname: idAsUserFieldname
#line 592 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2419 "parser_gen.cpp"
                    break;

                    case 88:  // predFieldname: argAsUserFieldname
#line 592 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2425 "parser_gen.cpp"
                    break;

                    case 89:  // predFieldname: invariableUserFieldname
#line 592 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2431 "parser_gen.cpp"
                    break;

                    case 90:  // invariableUserFieldname: "fieldname"
#line 595 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2439 "parser_gen.cpp"
                    break;

                    case 91:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 603 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2447 "parser_gen.cpp"
                    break;

                    case 92:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 606 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2455 "parser_gen.cpp"
                    break;

                    case 93:  // stageAsUserFieldname: STAGE_SKIP
#line 609 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2463 "parser_gen.cpp"
                    break;

                    case 94:  // stageAsUserFieldname: STAGE_LIMIT
#line 612 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2471 "parser_gen.cpp"
                    break;

                    case 95:  // stageAsUserFieldname: STAGE_PROJECT
#line 615 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2479 "parser_gen.cpp"
                    break;

                    case 96:  // stageAsUserFieldname: STAGE_SAMPLE
#line 618 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2487 "parser_gen.cpp"
                    break;

                    case 97:  // argAsUserFieldname: "coll argument"
#line 627 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2495 "parser_gen.cpp"
                    break;

                    case 98:  // argAsUserFieldname: "pipeline argument"
#line 630 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2503 "parser_gen.cpp"
                    break;

                    case 99:  // argAsUserFieldname: "size argument"
#line 633 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2511 "parser_gen.cpp"
                    break;

                    case 100:  // argAsUserFieldname: "input argument"
#line 636 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2519 "parser_gen.cpp"
                    break;

                    case 101:  // argAsUserFieldname: "to argument"
#line 639 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2527 "parser_gen.cpp"
                    break;

                    case 102:  // argAsUserFieldname: "onError argument"
#line 642 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2535 "parser_gen.cpp"
                    break;

                    case 103:  // argAsUserFieldname: "onNull argument"
#line 645 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2543 "parser_gen.cpp"
                    break;

                    case 104:  // argAsUserFieldname: "dateString argument"
#line 648 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2551 "parser_gen.cpp"
                    break;

                    case 105:  // argAsUserFieldname: "format argument"
#line 651 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2559 "parser_gen.cpp"
                    break;

                    case 106:  // argAsUserFieldname: "timezone argument"
#line 654 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2567 "parser_gen.cpp"
                    break;

                    case 107:  // argAsUserFieldname: "date argument"
#line 657 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2575 "parser_gen.cpp"
                    break;

                    case 108:  // argAsUserFieldname: "chars argument"
#line 660 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2583 "parser_gen.cpp"
                    break;

                    case 109:  // argAsUserFieldname: "regex argument"
#line 663 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2591 "parser_gen.cpp"
                    break;

                    case 110:  // argAsUserFieldname: "options argument"
#line 666 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2599 "parser_gen.cpp"
                    break;

                    case 111:  // argAsUserFieldname: "find argument"
#line 669 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2607 "parser_gen.cpp"
                    break;

                    case 112:  // argAsUserFieldname: "replacement argument"
#line 672 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2615 "parser_gen.cpp"
                    break;

                    case 113:  // argAsUserFieldname: "filter"
#line 675 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"filter"};
                    }
#line 2623 "parser_gen.cpp"
                    break;

                    case 114:  // argAsUserFieldname: "q"
#line 678 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"q"};
                    }
#line 2631 "parser_gen.cpp"
                    break;

                    case 115:  // aggExprAsUserFieldname: ADD
#line 686 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2639 "parser_gen.cpp"
                    break;

                    case 116:  // aggExprAsUserFieldname: ATAN2
#line 689 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2647 "parser_gen.cpp"
                    break;

                    case 117:  // aggExprAsUserFieldname: AND
#line 692 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2655 "parser_gen.cpp"
                    break;

                    case 118:  // aggExprAsUserFieldname: CONST_EXPR
#line 695 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2663 "parser_gen.cpp"
                    break;

                    case 119:  // aggExprAsUserFieldname: LITERAL
#line 698 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2671 "parser_gen.cpp"
                    break;

                    case 120:  // aggExprAsUserFieldname: OR
#line 701 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2679 "parser_gen.cpp"
                    break;

                    case 121:  // aggExprAsUserFieldname: NOT
#line 704 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2687 "parser_gen.cpp"
                    break;

                    case 122:  // aggExprAsUserFieldname: CMP
#line 707 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2695 "parser_gen.cpp"
                    break;

                    case 123:  // aggExprAsUserFieldname: EQ
#line 710 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2703 "parser_gen.cpp"
                    break;

                    case 124:  // aggExprAsUserFieldname: GT
#line 713 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2711 "parser_gen.cpp"
                    break;

                    case 125:  // aggExprAsUserFieldname: GTE
#line 716 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2719 "parser_gen.cpp"
                    break;

                    case 126:  // aggExprAsUserFieldname: LT
#line 719 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2727 "parser_gen.cpp"
                    break;

                    case 127:  // aggExprAsUserFieldname: LTE
#line 722 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2735 "parser_gen.cpp"
                    break;

                    case 128:  // aggExprAsUserFieldname: NE
#line 725 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2743 "parser_gen.cpp"
                    break;

                    case 129:  // aggExprAsUserFieldname: CONVERT
#line 728 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2751 "parser_gen.cpp"
                    break;

                    case 130:  // aggExprAsUserFieldname: TO_BOOL
#line 731 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2759 "parser_gen.cpp"
                    break;

                    case 131:  // aggExprAsUserFieldname: TO_DATE
#line 734 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2767 "parser_gen.cpp"
                    break;

                    case 132:  // aggExprAsUserFieldname: TO_DECIMAL
#line 737 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2775 "parser_gen.cpp"
                    break;

                    case 133:  // aggExprAsUserFieldname: TO_DOUBLE
#line 740 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2783 "parser_gen.cpp"
                    break;

                    case 134:  // aggExprAsUserFieldname: TO_INT
#line 743 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2791 "parser_gen.cpp"
                    break;

                    case 135:  // aggExprAsUserFieldname: TO_LONG
#line 746 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2799 "parser_gen.cpp"
                    break;

                    case 136:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 749 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2807 "parser_gen.cpp"
                    break;

                    case 137:  // aggExprAsUserFieldname: TO_STRING
#line 752 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2815 "parser_gen.cpp"
                    break;

                    case 138:  // aggExprAsUserFieldname: TYPE
#line 755 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2823 "parser_gen.cpp"
                    break;

                    case 139:  // aggExprAsUserFieldname: ABS
#line 758 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2831 "parser_gen.cpp"
                    break;

                    case 140:  // aggExprAsUserFieldname: CEIL
#line 761 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2839 "parser_gen.cpp"
                    break;

                    case 141:  // aggExprAsUserFieldname: DIVIDE
#line 764 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2847 "parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: EXPONENT
#line 767 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2855 "parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: FLOOR
#line 770 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2863 "parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: LN
#line 773 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2871 "parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: LOG
#line 776 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2879 "parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: LOGTEN
#line 779 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2887 "parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: MOD
#line 782 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2895 "parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: MULTIPLY
#line 785 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2903 "parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: POW
#line 788 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2911 "parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: ROUND
#line 791 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2919 "parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: SQRT
#line 794 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2927 "parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: SUBTRACT
#line 797 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2935 "parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: TRUNC
#line 800 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2943 "parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: CONCAT
#line 803 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2951 "parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 806 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2959 "parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 809 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2967 "parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 812 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2975 "parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 815 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2983 "parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: LTRIM
#line 818 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2991 "parser_gen.cpp"
                    break;

                    case 160:  // aggExprAsUserFieldname: META
#line 821 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 2999 "parser_gen.cpp"
                    break;

                    case 161:  // aggExprAsUserFieldname: REGEX_FIND
#line 824 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 3007 "parser_gen.cpp"
                    break;

                    case 162:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 827 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 3015 "parser_gen.cpp"
                    break;

                    case 163:  // aggExprAsUserFieldname: REGEX_MATCH
#line 830 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 3023 "parser_gen.cpp"
                    break;

                    case 164:  // aggExprAsUserFieldname: REPLACE_ONE
#line 833 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 3031 "parser_gen.cpp"
                    break;

                    case 165:  // aggExprAsUserFieldname: REPLACE_ALL
#line 836 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 3039 "parser_gen.cpp"
                    break;

                    case 166:  // aggExprAsUserFieldname: RTRIM
#line 839 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 3047 "parser_gen.cpp"
                    break;

                    case 167:  // aggExprAsUserFieldname: SPLIT
#line 842 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 3055 "parser_gen.cpp"
                    break;

                    case 168:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 845 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 3063 "parser_gen.cpp"
                    break;

                    case 169:  // aggExprAsUserFieldname: STR_LEN_CP
#line 848 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 3071 "parser_gen.cpp"
                    break;

                    case 170:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 851 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 3079 "parser_gen.cpp"
                    break;

                    case 171:  // aggExprAsUserFieldname: SUBSTR
#line 854 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 3087 "parser_gen.cpp"
                    break;

                    case 172:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 857 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 3095 "parser_gen.cpp"
                    break;

                    case 173:  // aggExprAsUserFieldname: SUBSTR_CP
#line 860 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 3103 "parser_gen.cpp"
                    break;

                    case 174:  // aggExprAsUserFieldname: TO_LOWER
#line 863 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 3111 "parser_gen.cpp"
                    break;

                    case 175:  // aggExprAsUserFieldname: TRIM
#line 866 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 3119 "parser_gen.cpp"
                    break;

                    case 176:  // aggExprAsUserFieldname: TO_UPPER
#line 869 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 3127 "parser_gen.cpp"
                    break;

                    case 177:  // aggExprAsUserFieldname: "allElementsTrue"
#line 872 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$allElementsTrue"};
                    }
#line 3135 "parser_gen.cpp"
                    break;

                    case 178:  // aggExprAsUserFieldname: "anyElementTrue"
#line 875 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$anyElementTrue"};
                    }
#line 3143 "parser_gen.cpp"
                    break;

                    case 179:  // aggExprAsUserFieldname: "setDifference"
#line 878 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setDifference"};
                    }
#line 3151 "parser_gen.cpp"
                    break;

                    case 180:  // aggExprAsUserFieldname: "setEquals"
#line 881 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setEquals"};
                    }
#line 3159 "parser_gen.cpp"
                    break;

                    case 181:  // aggExprAsUserFieldname: "setIntersection"
#line 884 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIntersection"};
                    }
#line 3167 "parser_gen.cpp"
                    break;

                    case 182:  // aggExprAsUserFieldname: "setIsSubset"
#line 887 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setIsSubset"};
                    }
#line 3175 "parser_gen.cpp"
                    break;

                    case 183:  // aggExprAsUserFieldname: "setUnion"
#line 890 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$setUnion"};
                    }
#line 3183 "parser_gen.cpp"
                    break;

                    case 184:  // string: "string"
#line 897 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 3191 "parser_gen.cpp"
                    break;

                    case 185:  // string: "randVal"
#line 902 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 3199 "parser_gen.cpp"
                    break;

                    case 186:  // string: "textScore"
#line 905 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 3207 "parser_gen.cpp"
                    break;

                    case 187:  // fieldPath: "$-prefixed string"
#line 911 "grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 3219 "parser_gen.cpp"
                    break;

                    case 188:  // variable: "$$-prefixed string"
#line 919 "grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 3232 "parser_gen.cpp"
                    break;

                    case 189:  // binary: "BinData"
#line 928 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 3240 "parser_gen.cpp"
                    break;

                    case 190:  // undefined: "undefined"
#line 934 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 3248 "parser_gen.cpp"
                    break;

                    case 191:  // objectId: "ObjectID"
#line 940 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 3256 "parser_gen.cpp"
                    break;

                    case 192:  // date: "Date"
#line 946 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3264 "parser_gen.cpp"
                    break;

                    case 193:  // null: "null"
#line 952 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3272 "parser_gen.cpp"
                    break;

                    case 194:  // regex: "regex"
#line 958 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3280 "parser_gen.cpp"
                    break;

                    case 195:  // dbPointer: "dbPointer"
#line 964 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3288 "parser_gen.cpp"
                    break;

                    case 196:  // javascript: "Code"
#line 970 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3296 "parser_gen.cpp"
                    break;

                    case 197:  // symbol: "Symbol"
#line 976 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3304 "parser_gen.cpp"
                    break;

                    case 198:  // javascriptWScope: "CodeWScope"
#line 982 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3312 "parser_gen.cpp"
                    break;

                    case 199:  // timestamp: "Timestamp"
#line 988 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3320 "parser_gen.cpp"
                    break;

                    case 200:  // minKey: "minKey"
#line 994 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3328 "parser_gen.cpp"
                    break;

                    case 201:  // maxKey: "maxKey"
#line 1000 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3336 "parser_gen.cpp"
                    break;

                    case 202:  // int: "arbitrary integer"
#line 1006 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3344 "parser_gen.cpp"
                    break;

                    case 203:  // int: "zero (int)"
#line 1009 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3352 "parser_gen.cpp"
                    break;

                    case 204:  // int: "1 (int)"
#line 1012 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3360 "parser_gen.cpp"
                    break;

                    case 205:  // int: "-1 (int)"
#line 1015 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3368 "parser_gen.cpp"
                    break;

                    case 206:  // long: "arbitrary long"
#line 1021 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3376 "parser_gen.cpp"
                    break;

                    case 207:  // long: "zero (long)"
#line 1024 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3384 "parser_gen.cpp"
                    break;

                    case 208:  // long: "1 (long)"
#line 1027 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3392 "parser_gen.cpp"
                    break;

                    case 209:  // long: "-1 (long)"
#line 1030 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3400 "parser_gen.cpp"
                    break;

                    case 210:  // double: "arbitrary double"
#line 1036 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3408 "parser_gen.cpp"
                    break;

                    case 211:  // double: "zero (double)"
#line 1039 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3416 "parser_gen.cpp"
                    break;

                    case 212:  // double: "1 (double)"
#line 1042 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3424 "parser_gen.cpp"
                    break;

                    case 213:  // double: "-1 (double)"
#line 1045 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3432 "parser_gen.cpp"
                    break;

                    case 214:  // decimal: "arbitrary decimal"
#line 1051 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3440 "parser_gen.cpp"
                    break;

                    case 215:  // decimal: "zero (decimal)"
#line 1054 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3448 "parser_gen.cpp"
                    break;

                    case 216:  // decimal: "1 (decimal)"
#line 1057 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3456 "parser_gen.cpp"
                    break;

                    case 217:  // decimal: "-1 (decimal)"
#line 1060 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3464 "parser_gen.cpp"
                    break;

                    case 218:  // bool: "true"
#line 1066 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3472 "parser_gen.cpp"
                    break;

                    case 219:  // bool: "false"
#line 1069 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3480 "parser_gen.cpp"
                    break;

                    case 220:  // simpleValue: string
#line 1075 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3486 "parser_gen.cpp"
                    break;

                    case 221:  // simpleValue: fieldPath
#line 1076 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3492 "parser_gen.cpp"
                    break;

                    case 222:  // simpleValue: variable
#line 1077 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3498 "parser_gen.cpp"
                    break;

                    case 223:  // simpleValue: binary
#line 1078 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3504 "parser_gen.cpp"
                    break;

                    case 224:  // simpleValue: undefined
#line 1079 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3510 "parser_gen.cpp"
                    break;

                    case 225:  // simpleValue: objectId
#line 1080 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3516 "parser_gen.cpp"
                    break;

                    case 226:  // simpleValue: date
#line 1081 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3522 "parser_gen.cpp"
                    break;

                    case 227:  // simpleValue: null
#line 1082 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3528 "parser_gen.cpp"
                    break;

                    case 228:  // simpleValue: regex
#line 1083 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3534 "parser_gen.cpp"
                    break;

                    case 229:  // simpleValue: dbPointer
#line 1084 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3540 "parser_gen.cpp"
                    break;

                    case 230:  // simpleValue: javascript
#line 1085 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3546 "parser_gen.cpp"
                    break;

                    case 231:  // simpleValue: symbol
#line 1086 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3552 "parser_gen.cpp"
                    break;

                    case 232:  // simpleValue: javascriptWScope
#line 1087 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3558 "parser_gen.cpp"
                    break;

                    case 233:  // simpleValue: int
#line 1088 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3564 "parser_gen.cpp"
                    break;

                    case 234:  // simpleValue: long
#line 1089 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3570 "parser_gen.cpp"
                    break;

                    case 235:  // simpleValue: double
#line 1090 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3576 "parser_gen.cpp"
                    break;

                    case 236:  // simpleValue: decimal
#line 1091 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3582 "parser_gen.cpp"
                    break;

                    case 237:  // simpleValue: bool
#line 1092 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3588 "parser_gen.cpp"
                    break;

                    case 238:  // simpleValue: timestamp
#line 1093 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3594 "parser_gen.cpp"
                    break;

                    case 239:  // simpleValue: minKey
#line 1094 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3600 "parser_gen.cpp"
                    break;

                    case 240:  // simpleValue: maxKey
#line 1095 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3606 "parser_gen.cpp"
                    break;

                    case 241:  // expressions: %empty
#line 1102 "grammar.yy"
                    {
                    }
#line 3612 "parser_gen.cpp"
                    break;

                    case 242:  // expressions: expression expressions
#line 1103 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3621 "parser_gen.cpp"
                    break;

                    case 243:  // expression: simpleValue
#line 1110 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3627 "parser_gen.cpp"
                    break;

                    case 244:  // expression: compoundExpression
#line 1110 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3633 "parser_gen.cpp"
                    break;

                    case 245:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1114 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3641 "parser_gen.cpp"
                    break;

                    case 246:  // compoundExpression: expressionArray
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3647 "parser_gen.cpp"
                    break;

                    case 247:  // compoundExpression: expressionObject
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3653 "parser_gen.cpp"
                    break;

                    case 248:  // compoundExpression: maths
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3659 "parser_gen.cpp"
                    break;

                    case 249:  // compoundExpression: boolExps
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3665 "parser_gen.cpp"
                    break;

                    case 250:  // compoundExpression: literalEscapes
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3671 "parser_gen.cpp"
                    break;

                    case 251:  // compoundExpression: compExprs
#line 1119 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3677 "parser_gen.cpp"
                    break;

                    case 252:  // compoundExpression: typeExpression
#line 1120 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3683 "parser_gen.cpp"
                    break;

                    case 253:  // compoundExpression: stringExps
#line 1120 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3689 "parser_gen.cpp"
                    break;

                    case 254:  // compoundExpression: setExpression
#line 1120 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3695 "parser_gen.cpp"
                    break;

                    case 255:  // expressionArray: "array" expressions "end of array"
#line 1126 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3703 "parser_gen.cpp"
                    break;

                    case 256:  // expressionObject: "object" expressionFields "end of object"
#line 1134 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3711 "parser_gen.cpp"
                    break;

                    case 257:  // expressionFields: %empty
#line 1140 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3719 "parser_gen.cpp"
                    break;

                    case 258:  // expressionFields: expressionFields expressionField
#line 1143 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3728 "parser_gen.cpp"
                    break;

                    case 259:  // expressionField: expressionFieldname expression
#line 1150 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3736 "parser_gen.cpp"
                    break;

                    case 260:  // expressionFieldname: invariableUserFieldname
#line 1157 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3742 "parser_gen.cpp"
                    break;

                    case 261:  // expressionFieldname: stageAsUserFieldname
#line 1157 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3748 "parser_gen.cpp"
                    break;

                    case 262:  // expressionFieldname: argAsUserFieldname
#line 1157 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3754 "parser_gen.cpp"
                    break;

                    case 263:  // expressionFieldname: idAsUserFieldname
#line 1157 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3760 "parser_gen.cpp"
                    break;

                    case 264:  // idAsUserFieldname: ID
#line 1161 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3768 "parser_gen.cpp"
                    break;

                    case 265:  // maths: add
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3774 "parser_gen.cpp"
                    break;

                    case 266:  // maths: atan2
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3780 "parser_gen.cpp"
                    break;

                    case 267:  // maths: abs
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3786 "parser_gen.cpp"
                    break;

                    case 268:  // maths: ceil
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3792 "parser_gen.cpp"
                    break;

                    case 269:  // maths: divide
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3798 "parser_gen.cpp"
                    break;

                    case 270:  // maths: exponent
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3804 "parser_gen.cpp"
                    break;

                    case 271:  // maths: floor
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3810 "parser_gen.cpp"
                    break;

                    case 272:  // maths: ln
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3816 "parser_gen.cpp"
                    break;

                    case 273:  // maths: log
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3822 "parser_gen.cpp"
                    break;

                    case 274:  // maths: logten
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3828 "parser_gen.cpp"
                    break;

                    case 275:  // maths: mod
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3834 "parser_gen.cpp"
                    break;

                    case 276:  // maths: multiply
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3840 "parser_gen.cpp"
                    break;

                    case 277:  // maths: pow
#line 1167 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3846 "parser_gen.cpp"
                    break;

                    case 278:  // maths: round
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3852 "parser_gen.cpp"
                    break;

                    case 279:  // maths: sqrt
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3858 "parser_gen.cpp"
                    break;

                    case 280:  // maths: subtract
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3864 "parser_gen.cpp"
                    break;

                    case 281:  // maths: trunc
#line 1168 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3870 "parser_gen.cpp"
                    break;

                    case 282:  // add: "object" ADD expressionArray "end of object"
#line 1172 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3879 "parser_gen.cpp"
                    break;

                    case 283:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1179 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3888 "parser_gen.cpp"
                    break;

                    case 284:  // abs: "object" ABS expression "end of object"
#line 1185 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3896 "parser_gen.cpp"
                    break;

                    case 285:  // ceil: "object" CEIL expression "end of object"
#line 1190 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3904 "parser_gen.cpp"
                    break;

                    case 286:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1195 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3913 "parser_gen.cpp"
                    break;

                    case 287:  // exponent: "object" EXPONENT expression "end of object"
#line 1201 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3921 "parser_gen.cpp"
                    break;

                    case 288:  // floor: "object" FLOOR expression "end of object"
#line 1206 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3929 "parser_gen.cpp"
                    break;

                    case 289:  // ln: "object" LN expression "end of object"
#line 1211 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3937 "parser_gen.cpp"
                    break;

                    case 290:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1216 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3946 "parser_gen.cpp"
                    break;

                    case 291:  // logten: "object" LOGTEN expression "end of object"
#line 1222 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3954 "parser_gen.cpp"
                    break;

                    case 292:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1227 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3963 "parser_gen.cpp"
                    break;

                    case 293:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1233 "grammar.yy"
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
#line 3975 "parser_gen.cpp"
                    break;

                    case 294:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1242 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3984 "parser_gen.cpp"
                    break;

                    case 295:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1248 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3993 "parser_gen.cpp"
                    break;

                    case 296:  // sqrt: "object" SQRT expression "end of object"
#line 1254 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4001 "parser_gen.cpp"
                    break;

                    case 297:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1259 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4010 "parser_gen.cpp"
                    break;

                    case 298:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1265 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4019 "parser_gen.cpp"
                    break;

                    case 299:  // boolExps: and
#line 1271 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4025 "parser_gen.cpp"
                    break;

                    case 300:  // boolExps: or
#line 1271 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4031 "parser_gen.cpp"
                    break;

                    case 301:  // boolExps: not
#line 1271 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4037 "parser_gen.cpp"
                    break;

                    case 302:  // and: "object" AND expressionArray "end of object"
#line 1275 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4046 "parser_gen.cpp"
                    break;

                    case 303:  // or: "object" OR expressionArray "end of object"
#line 1282 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4055 "parser_gen.cpp"
                    break;

                    case 304:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1289 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4064 "parser_gen.cpp"
                    break;

                    case 305:  // stringExps: concat
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4070 "parser_gen.cpp"
                    break;

                    case 306:  // stringExps: dateFromString
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4076 "parser_gen.cpp"
                    break;

                    case 307:  // stringExps: dateToString
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4082 "parser_gen.cpp"
                    break;

                    case 308:  // stringExps: indexOfBytes
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4088 "parser_gen.cpp"
                    break;

                    case 309:  // stringExps: indexOfCP
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4094 "parser_gen.cpp"
                    break;

                    case 310:  // stringExps: ltrim
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4100 "parser_gen.cpp"
                    break;

                    case 311:  // stringExps: regexFind
#line 1296 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4106 "parser_gen.cpp"
                    break;

                    case 312:  // stringExps: regexFindAll
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4112 "parser_gen.cpp"
                    break;

                    case 313:  // stringExps: regexMatch
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4118 "parser_gen.cpp"
                    break;

                    case 314:  // stringExps: replaceOne
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4124 "parser_gen.cpp"
                    break;

                    case 315:  // stringExps: replaceAll
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4130 "parser_gen.cpp"
                    break;

                    case 316:  // stringExps: rtrim
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4136 "parser_gen.cpp"
                    break;

                    case 317:  // stringExps: split
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4142 "parser_gen.cpp"
                    break;

                    case 318:  // stringExps: strLenBytes
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4148 "parser_gen.cpp"
                    break;

                    case 319:  // stringExps: strLenCP
#line 1297 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4154 "parser_gen.cpp"
                    break;

                    case 320:  // stringExps: strcasecmp
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4160 "parser_gen.cpp"
                    break;

                    case 321:  // stringExps: substr
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4166 "parser_gen.cpp"
                    break;

                    case 322:  // stringExps: substrBytes
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4172 "parser_gen.cpp"
                    break;

                    case 323:  // stringExps: substrCP
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4178 "parser_gen.cpp"
                    break;

                    case 324:  // stringExps: toLower
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4184 "parser_gen.cpp"
                    break;

                    case 325:  // stringExps: trim
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4190 "parser_gen.cpp"
                    break;

                    case 326:  // stringExps: toUpper
#line 1298 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4196 "parser_gen.cpp"
                    break;

                    case 327:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1302 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 4208 "parser_gen.cpp"
                    break;

                    case 328:  // formatArg: %empty
#line 1312 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 4216 "parser_gen.cpp"
                    break;

                    case 329:  // formatArg: "format argument" expression
#line 1315 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4224 "parser_gen.cpp"
                    break;

                    case 330:  // timezoneArg: %empty
#line 1321 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 4232 "parser_gen.cpp"
                    break;

                    case 331:  // timezoneArg: "timezone argument" expression
#line 1324 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4240 "parser_gen.cpp"
                    break;

                    case 332:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1331 "grammar.yy"
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
#line 4250 "parser_gen.cpp"
                    break;

                    case 333:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1340 "grammar.yy"
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
#line 4260 "parser_gen.cpp"
                    break;

                    case 334:  // exprZeroToTwo: %empty
#line 1348 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4268 "parser_gen.cpp"
                    break;

                    case 335:  // exprZeroToTwo: expression
#line 1351 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4276 "parser_gen.cpp"
                    break;

                    case 336:  // exprZeroToTwo: expression expression
#line 1354 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4284 "parser_gen.cpp"
                    break;

                    case 337:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 1361 "grammar.yy"
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
#line 4296 "parser_gen.cpp"
                    break;

                    case 338:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 1372 "grammar.yy"
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
#line 4308 "parser_gen.cpp"
                    break;

                    case 339:  // charsArg: %empty
#line 1382 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4316 "parser_gen.cpp"
                    break;

                    case 340:  // charsArg: "chars argument" expression
#line 1385 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4324 "parser_gen.cpp"
                    break;

                    case 341:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1391 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4334 "parser_gen.cpp"
                    break;

                    case 342:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1399 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4344 "parser_gen.cpp"
                    break;

                    case 343:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 1407 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4354 "parser_gen.cpp"
                    break;

                    case 344:  // optionsArg: %empty
#line 1415 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4362 "parser_gen.cpp"
                    break;

                    case 345:  // optionsArg: "options argument" expression
#line 1418 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4370 "parser_gen.cpp"
                    break;

                    case 346:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 1423 "grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4382 "parser_gen.cpp"
                    break;

                    case 347:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 1432 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4390 "parser_gen.cpp"
                    break;

                    case 348:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 1438 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4398 "parser_gen.cpp"
                    break;

                    case 349:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 1444 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4406 "parser_gen.cpp"
                    break;

                    case 350:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1451 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4417 "parser_gen.cpp"
                    break;

                    case 351:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1461 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4428 "parser_gen.cpp"
                    break;

                    case 352:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 1470 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4437 "parser_gen.cpp"
                    break;

                    case 353:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 1477 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4446 "parser_gen.cpp"
                    break;

                    case 354:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 1484 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4455 "parser_gen.cpp"
                    break;

                    case 355:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 1492 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4464 "parser_gen.cpp"
                    break;

                    case 356:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 1500 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4473 "parser_gen.cpp"
                    break;

                    case 357:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 1508 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4482 "parser_gen.cpp"
                    break;

                    case 358:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 1516 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4491 "parser_gen.cpp"
                    break;

                    case 359:  // toLower: "object" TO_LOWER expression "end of object"
#line 1523 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4499 "parser_gen.cpp"
                    break;

                    case 360:  // toUpper: "object" TO_UPPER expression "end of object"
#line 1529 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4507 "parser_gen.cpp"
                    break;

                    case 361:  // metaSortKeyword: "randVal"
#line 1535 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4515 "parser_gen.cpp"
                    break;

                    case 362:  // metaSortKeyword: "textScore"
#line 1538 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4523 "parser_gen.cpp"
                    break;

                    case 363:  // metaSort: "object" META metaSortKeyword "end of object"
#line 1544 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4531 "parser_gen.cpp"
                    break;

                    case 364:  // sortSpecs: "object" specList "end of object"
#line 1550 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4539 "parser_gen.cpp"
                    break;

                    case 365:  // specList: %empty
#line 1555 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4547 "parser_gen.cpp"
                    break;

                    case 366:  // specList: specList sortSpec
#line 1558 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4556 "parser_gen.cpp"
                    break;

                    case 367:  // oneOrNegOne: "1 (int)"
#line 1565 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4564 "parser_gen.cpp"
                    break;

                    case 368:  // oneOrNegOne: "-1 (int)"
#line 1568 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4572 "parser_gen.cpp"
                    break;

                    case 369:  // oneOrNegOne: "1 (long)"
#line 1571 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4580 "parser_gen.cpp"
                    break;

                    case 370:  // oneOrNegOne: "-1 (long)"
#line 1574 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4588 "parser_gen.cpp"
                    break;

                    case 371:  // oneOrNegOne: "1 (double)"
#line 1577 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4596 "parser_gen.cpp"
                    break;

                    case 372:  // oneOrNegOne: "-1 (double)"
#line 1580 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 4604 "parser_gen.cpp"
                    break;

                    case 373:  // oneOrNegOne: "1 (decimal)"
#line 1583 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 4612 "parser_gen.cpp"
                    break;

                    case 374:  // oneOrNegOne: "-1 (decimal)"
#line 1586 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 4620 "parser_gen.cpp"
                    break;

                    case 375:  // sortSpec: valueFieldname metaSort
#line 1591 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4628 "parser_gen.cpp"
                    break;

                    case 376:  // sortSpec: valueFieldname oneOrNegOne
#line 1593 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4636 "parser_gen.cpp"
                    break;

                    case 377:  // setExpression: allElementsTrue
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4642 "parser_gen.cpp"
                    break;

                    case 378:  // setExpression: anyElementTrue
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4648 "parser_gen.cpp"
                    break;

                    case 379:  // setExpression: setDifference
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4654 "parser_gen.cpp"
                    break;

                    case 380:  // setExpression: setEquals
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4660 "parser_gen.cpp"
                    break;

                    case 381:  // setExpression: setIntersection
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4666 "parser_gen.cpp"
                    break;

                    case 382:  // setExpression: setIsSubset
#line 1599 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4672 "parser_gen.cpp"
                    break;

                    case 383:  // setExpression: setUnion
#line 1600 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4678 "parser_gen.cpp"
                    break;

                    case 384:  // allElementsTrue: "object" "allElementsTrue" "array" expression
                               // "end of array" "end of object"
#line 1604 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::allElementsTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 4686 "parser_gen.cpp"
                    break;

                    case 385:  // anyElementTrue: "object" "anyElementTrue" "array" expression "end
                               // of array" "end of object"
#line 1610 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{
                            CNode::ObjectChildren{{KeyFieldname::anyElementTrue,
                                                   CNode{YY_MOVE(yystack_[2].value.as<CNode>())}}}};
                    }
#line 4694 "parser_gen.cpp"
                    break;

                    case 386:  // setDifference: "object" "setDifference" exprFixedTwoArg "end of
                               // object"
#line 1616 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setDifference, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4703 "parser_gen.cpp"
                    break;

                    case 387:  // setEquals: "object" "setEquals" "array" expression expression
                               // expressions "end of array" "end of object"
#line 1624 "grammar.yy"
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
#line 4715 "parser_gen.cpp"
                    break;

                    case 388:  // setIntersection: "object" "setIntersection" "array" expression
                               // expression expressions "end of array" "end of object"
#line 1635 "grammar.yy"
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
#line 4727 "parser_gen.cpp"
                    break;

                    case 389:  // setIsSubset: "object" "setIsSubset" exprFixedTwoArg "end of
                               // object"
#line 1645 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::setIsSubset, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4736 "parser_gen.cpp"
                    break;

                    case 390:  // setUnion: "object" "setUnion" "array" expression expression
                               // expressions "end of array" "end of object"
#line 1653 "grammar.yy"
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
#line 4748 "parser_gen.cpp"
                    break;

                    case 391:  // literalEscapes: const
#line 1663 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4754 "parser_gen.cpp"
                    break;

                    case 392:  // literalEscapes: literal
#line 1663 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4760 "parser_gen.cpp"
                    break;

                    case 393:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 1667 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4769 "parser_gen.cpp"
                    break;

                    case 394:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 1674 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4778 "parser_gen.cpp"
                    break;

                    case 395:  // value: simpleValue
#line 1681 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4784 "parser_gen.cpp"
                    break;

                    case 396:  // value: compoundValue
#line 1681 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4790 "parser_gen.cpp"
                    break;

                    case 397:  // compoundValue: valueArray
#line 1685 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4796 "parser_gen.cpp"
                    break;

                    case 398:  // compoundValue: valueObject
#line 1685 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4802 "parser_gen.cpp"
                    break;

                    case 399:  // valueArray: "array" values "end of array"
#line 1689 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4810 "parser_gen.cpp"
                    break;

                    case 400:  // values: %empty
#line 1695 "grammar.yy"
                    {
                    }
#line 4816 "parser_gen.cpp"
                    break;

                    case 401:  // values: value values
#line 1696 "grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4825 "parser_gen.cpp"
                    break;

                    case 402:  // valueObject: "object" valueFields "end of object"
#line 1703 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4833 "parser_gen.cpp"
                    break;

                    case 403:  // valueFields: %empty
#line 1709 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4841 "parser_gen.cpp"
                    break;

                    case 404:  // valueFields: valueFields valueField
#line 1712 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4850 "parser_gen.cpp"
                    break;

                    case 405:  // valueField: valueFieldname value
#line 1719 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4858 "parser_gen.cpp"
                    break;

                    case 406:  // valueFieldname: invariableUserFieldname
#line 1726 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4864 "parser_gen.cpp"
                    break;

                    case 407:  // valueFieldname: stageAsUserFieldname
#line 1727 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4870 "parser_gen.cpp"
                    break;

                    case 408:  // valueFieldname: argAsUserFieldname
#line 1728 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4876 "parser_gen.cpp"
                    break;

                    case 409:  // valueFieldname: aggExprAsUserFieldname
#line 1729 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4882 "parser_gen.cpp"
                    break;

                    case 410:  // valueFieldname: idAsUserFieldname
#line 1730 "grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4888 "parser_gen.cpp"
                    break;

                    case 411:  // compExprs: cmp
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4894 "parser_gen.cpp"
                    break;

                    case 412:  // compExprs: eq
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4900 "parser_gen.cpp"
                    break;

                    case 413:  // compExprs: gt
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4906 "parser_gen.cpp"
                    break;

                    case 414:  // compExprs: gte
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4912 "parser_gen.cpp"
                    break;

                    case 415:  // compExprs: lt
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4918 "parser_gen.cpp"
                    break;

                    case 416:  // compExprs: lte
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4924 "parser_gen.cpp"
                    break;

                    case 417:  // compExprs: ne
#line 1733 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4930 "parser_gen.cpp"
                    break;

                    case 418:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 1735 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4939 "parser_gen.cpp"
                    break;

                    case 419:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 1740 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4948 "parser_gen.cpp"
                    break;

                    case 420:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 1745 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4957 "parser_gen.cpp"
                    break;

                    case 421:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 1750 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4966 "parser_gen.cpp"
                    break;

                    case 422:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 1755 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4975 "parser_gen.cpp"
                    break;

                    case 423:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 1760 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4984 "parser_gen.cpp"
                    break;

                    case 424:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 1765 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4993 "parser_gen.cpp"
                    break;

                    case 425:  // typeExpression: convert
#line 1771 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4999 "parser_gen.cpp"
                    break;

                    case 426:  // typeExpression: toBool
#line 1772 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5005 "parser_gen.cpp"
                    break;

                    case 427:  // typeExpression: toDate
#line 1773 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5011 "parser_gen.cpp"
                    break;

                    case 428:  // typeExpression: toDecimal
#line 1774 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5017 "parser_gen.cpp"
                    break;

                    case 429:  // typeExpression: toDouble
#line 1775 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5023 "parser_gen.cpp"
                    break;

                    case 430:  // typeExpression: toInt
#line 1776 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5029 "parser_gen.cpp"
                    break;

                    case 431:  // typeExpression: toLong
#line 1777 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5035 "parser_gen.cpp"
                    break;

                    case 432:  // typeExpression: toObjectId
#line 1778 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5041 "parser_gen.cpp"
                    break;

                    case 433:  // typeExpression: toString
#line 1779 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5047 "parser_gen.cpp"
                    break;

                    case 434:  // typeExpression: type
#line 1780 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 5053 "parser_gen.cpp"
                    break;

                    case 435:  // onErrorArg: %empty
#line 1785 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 5061 "parser_gen.cpp"
                    break;

                    case 436:  // onErrorArg: "onError argument" expression
#line 1788 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5069 "parser_gen.cpp"
                    break;

                    case 437:  // onNullArg: %empty
#line 1795 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 5077 "parser_gen.cpp"
                    break;

                    case 438:  // onNullArg: "onNull argument" expression
#line 1798 "grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 5085 "parser_gen.cpp"
                    break;

                    case 439:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 1805 "grammar.yy"
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
#line 5096 "parser_gen.cpp"
                    break;

                    case 440:  // toBool: "object" TO_BOOL expression "end of object"
#line 1814 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5104 "parser_gen.cpp"
                    break;

                    case 441:  // toDate: "object" TO_DATE expression "end of object"
#line 1819 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5112 "parser_gen.cpp"
                    break;

                    case 442:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 1824 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5120 "parser_gen.cpp"
                    break;

                    case 443:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 1829 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5128 "parser_gen.cpp"
                    break;

                    case 444:  // toInt: "object" TO_INT expression "end of object"
#line 1834 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5136 "parser_gen.cpp"
                    break;

                    case 445:  // toLong: "object" TO_LONG expression "end of object"
#line 1839 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5144 "parser_gen.cpp"
                    break;

                    case 446:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 1844 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5152 "parser_gen.cpp"
                    break;

                    case 447:  // toString: "object" TO_STRING expression "end of object"
#line 1849 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5160 "parser_gen.cpp"
                    break;

                    case 448:  // type: "object" TYPE expression "end of object"
#line 1854 "grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 5168 "parser_gen.cpp"
                    break;


#line 5172 "parser_gen.cpp"

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


const short ParserGen::yypact_ninf_ = -680;

const signed char ParserGen::yytable_ninf_ = -1;

const short ParserGen::yypact_[] = {
    23,   -58,  -47,  -58,  -58,  -40,  61,   -680, -680, -35,  -680, -680, -680, -680, -680, -680,
    923,  27,   33,   439,  -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, 1061, -680, -680, -680,
    -680, -680, -11,  -2,   106,  4,    16,   106,  -680, 38,   -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, 188,  -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -58,  70,   -680, -680, -680, -680, -680, -680, 108,  -680, 138,  54,   -35,  -680, -680, -680,
    -680, -680, -680, -680, -680, 85,   -680, -680, -18,  -680, -680, 573,  106,  -57,  -680, -680,
    -61,  -680, -91,  -680, -680, -23,  -680, 1173, 1173, -680, -680, -680, -680, -680, 109,  137,
    -680, -680, 111,  86,   -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, 253,  809,  -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -5,   -680, -680, -680, 253,  -680,
    113,  253,  63,   67,   63,   68,   69,   253,  69,   74,   75,   -680, -680, -680, 76,   69,
    253,  253,  69,   69,   77,   78,   79,   253,  80,   253,  69,   69,   -680, 81,   87,   69,
    88,   63,   89,   -680, -680, -680, -680, -680, 90,   -680, 69,   94,   97,   69,   98,   103,
    253,  104,  253,  253,  110,  115,  118,  131,  253,  253,  253,  253,  253,  253,  253,  253,
    253,  253,  -680, 135,  253,  944,  160,  -17,  -680, -680, 187,  189,  253,  190,  253,  253,
    196,  199,  200,  253,  949,  168,  223,  237,  253,  202,  203,  208,  210,  212,  253,  253,
    949,  213,  253,  215,  216,  217,  256,  253,  253,  219,  253,  221,  253,  225,  260,  231,
    232,  266,  267,  253,  256,  235,  253,  253,  238,  253,  253,  239,  253,  241,  243,  253,
    253,  253,  253,  252,  254,  255,  257,  259,  265,  268,  272,  273,  274,  256,  253,  275,
    -680, 253,  -680, -680, -680, -680, -680, -680, -680, -680, -680, 277,  -680, 279,  253,  -680,
    -680, -680, 280,  949,  -680, 282,  -680, -680, -680, -680, 253,  253,  253,  253,  -680, -680,
    -680, -680, -680, 253,  253,  283,  -680, 253,  -680, -680, -680, 253,  284,  253,  253,  -680,
    286,  -680, 253,  -680, 253,  -680, -680, 253,  253,  253,  287,  -680, 253,  253,  -680, 253,
    253,  -680, 253,  -680, -680, 253,  253,  253,  253,  -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, 297,  253,  -680, -680, 288,  289,  291,  292,  949,  301,  691,  296,  317,
    299,  299,  302,  253,  253,  306,  309,  -680, 253,  310,  253,  311,  313,  343,  347,  348,
    323,  253,  253,  253,  253,  324,  327,  253,  253,  253,  349,  253,  350,  -680, -680, -680,
    -680, -680, -680, -680, 949,  -680, -680, 253,  379,  253,  371,  371,  353,  253,  355,  356,
    -680, 357,  358,  359,  361,  -680, 362,  253,  380,  253,  253,  363,  364,  366,  367,  368,
    369,  370,  372,  373,  374,  375,  376,  377,  -680, -680, 253,  387,  -680, 253,  317,  379,
    -680, -680, 381,  382,  -680, 383,  -680, 384,  -680, -680, 253,  397,  402,  -680, 385,  386,
    388,  389,  -680, -680, 391,  395,  414,  -680, 418,  -680, -680, 253,  -680, 379,  422,  -680,
    -680, -680, -680, 423,  253,  253,  -680, -680, -680, -680, -680, -680, -680, -680, 431,  432,
    433,  -680, 435,  436,  437,  438,  -680, 448,  449,  -680, -680, -680, -680};

const short ParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   0,   0,   70,  3,   8,   2,   5,   4,   365, 6,   1,   0,   0,   0,
    0,   82,  108, 97,  107, 104, 113, 111, 105, 100, 102, 103, 110, 98,  114, 109, 112, 99,  106,
    101, 69,  264, 84,  83,  90,  0,   88,  89,  87,  71,  73,  0,   0,   0,   0,   0,   0,   10,
    0,   12,  13,  14,  15,  16,  17,  7,   139, 115, 177, 117, 178, 116, 140, 122, 154, 118, 129,
    155, 156, 141, 364, 123, 142, 143, 124, 125, 157, 158, 119, 144, 145, 146, 126, 127, 159, 160,
    147, 148, 128, 121, 120, 149, 161, 162, 163, 165, 164, 150, 166, 179, 180, 181, 182, 183, 167,
    151, 91,  94,  95,  96,  93,  92,  170, 168, 169, 171, 172, 173, 152, 130, 131, 132, 133, 134,
    135, 174, 136, 137, 176, 175, 153, 138, 407, 408, 409, 406, 410, 0,   366, 219, 218, 217, 216,
    215, 213, 212, 211, 205, 204, 203, 209, 208, 207, 185, 76,  186, 184, 189, 190, 191, 192, 193,
    194, 195, 196, 197, 198, 202, 206, 210, 214, 199, 200, 201, 187, 188, 229, 230, 231, 232, 233,
    238, 234, 235, 236, 239, 240, 220, 221, 223, 224, 225, 237, 226, 227, 228, 74,  222, 72,  0,
    0,   21,  22,  23,  24,  26,  28,  0,   25,  0,   0,   8,   374, 373, 372, 371, 368, 367, 370,
    369, 0,   375, 376, 0,   85,  19,  0,   0,   0,   11,  9,   0,   75,  0,   77,  78,  0,   27,
    0,   0,   66,  67,  68,  65,  29,  0,   0,   361, 362, 0,   0,   79,  81,  86,  60,  59,  56,
    55,  58,  52,  51,  54,  44,  43,  46,  48,  47,  50,  241, 257, 45,  49,  53,  57,  39,  40,
    41,  42,  61,  62,  63,  32,  33,  34,  35,  36,  37,  38,  30,  64,  246, 247, 248, 265, 266,
    249, 299, 300, 301, 250, 391, 392, 253, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315,
    316, 317, 318, 319, 320, 321, 322, 323, 324, 326, 325, 251, 411, 412, 413, 414, 415, 416, 417,
    252, 425, 426, 427, 428, 429, 430, 431, 432, 433, 434, 267, 268, 269, 270, 271, 272, 273, 274,
    275, 276, 277, 278, 279, 280, 281, 254, 377, 378, 379, 380, 381, 382, 383, 31,  18,  0,   363,
    76,  243, 241, 244, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  10,  10,  0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   0,
    0,   10,  10,  10,  10,  10,  0,   10,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   10,  0,   0,   0,   0,   0,
    242, 255, 0,   0,   0,   0,   0,   0,   0,   0,   0,   241, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   339, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   339, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   339, 0,   0,   256, 0,   261, 262,
    260, 263, 258, 20,  80,  284, 282, 0,   302, 0,   0,   283, 285, 418, 0,   400, 403, 0,   395,
    396, 397, 398, 0,   0,   0,   0,   419, 287, 288, 420, 421, 0,   0,   0,   289, 0,   291, 422,
    423, 0,   0,   0,   0,   424, 0,   303, 0,   347, 0,   348, 349, 0,   0,   0,   0,   386, 0,
    0,   389, 0,   0,   296, 0,   353, 354, 0,   0,   0,   0,   440, 441, 442, 443, 444, 445, 359,
    446, 447, 360, 0,   0,   448, 259, 0,   0,   0,   0,   400, 0,   0,   0,   435, 328, 328, 0,
    334, 334, 0,   0,   340, 0,   0,   241, 0,   0,   344, 0,   0,   0,   0,   241, 241, 241, 0,
    0,   0,   0,   0,   0,   0,   0,   384, 385, 245, 327, 401, 399, 402, 0,   404, 393, 0,   437,
    0,   330, 330, 0,   335, 0,   0,   394, 0,   0,   0,   0,   304, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   405, 436, 0,   0,   329, 0,   435,
    437, 286, 336, 0,   0,   290, 0,   292, 0,   294, 345, 0,   0,   0,   295, 0,   0,   0,   0,
    352, 355, 0,   0,   0,   297, 0,   298, 438, 0,   331, 437, 0,   337, 338, 341, 293, 0,   0,
    0,   342, 387, 388, 390, 356, 357, 358, 343, 0,   0,   0,   346, 0,   0,   0,   0,   333, 0,
    0,   439, 332, 351, 350};

const short ParserGen::yypgoto_[] = {
    -680, -680, -680, -224, -680, -15,  172,  -14,  -13,  -188, -680, -680, -680, -195, -167,
    -165, -156, -34,  -153, -32,  -46,  -25,  -149, -147, -463, -158, -680, -142, -139, -137,
    -680, -122, -113, -201, -44,  -680, -680, -680, -680, -680, -680, 206,  -680, -680, -680,
    -680, -680, -680, -680, -680, 248,  -43,  -375, -107, -62,  -355, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -278, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680, -680,
    -680, -680, -680, -680, -680, -680, -200, -679, -124, -152, -449, -680, -374, -108, -92,
    -680, -680, -680, -680, -680, -680, -680, -680, 10,   -680, 157,  -680, -680, -680, -680,
    303,  -680, -680, -680, -680, -680, -680, -680, -680, -680, -52,  -680};

const short ParserGen::yydefgoto_[] = {
    -1,  248, 529, 141, 44,  142, 143, 144, 145, 146, 253, 534, 661, 185, 186, 187, 188, 189,
    190, 191, 192, 193, 194, 195, 619, 196, 197, 198, 199, 200, 201, 202, 203, 204, 381, 551,
    552, 553, 621, 206, 10,  18,  57,  58,  59,  60,  61,  62,  63,  235, 297, 214, 382, 383,
    464, 299, 300, 453, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314,
    315, 316, 317, 318, 319, 320, 493, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331,
    332, 333, 334, 335, 336, 337, 338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363, 364, 365, 366, 367,
    664, 699, 666, 702, 572, 680, 384, 620, 670, 368, 369, 370, 371, 372, 373, 374, 375, 8,
    16,  232, 207, 245, 48,  49,  243, 244, 50,  14,  19,  230, 231, 258, 147, 6,   494, 219};

const short ParserGen::yytable_[] = {
    205, 45,  46,  47,  218, 549, 212, 259, 456, 212, 458, 249, 217, 11,  12,  256, 465, 565, 210,
    162, 211, 210, 261, 211, 734, 474, 475, 213, 241, 536, 213, 459, 481, 461, 483, 1,   171, 153,
    154, 155, 7,   260, 2,   3,   4,   257, 296, 296, 5,   164, 9,   586, 283, 283, 751, 242, 242,
    507, 13,  509, 510, 15,  491, 17,  165, 515, 516, 517, 518, 519, 520, 521, 522, 523, 524, 7,
    611, 527, 64,  255, 284, 284, 285, 285, 220, 539, 208, 541, 542, 290, 290, 286, 286, 546, 287,
    287, 209, 557, 288, 288, 289, 289, 215, 563, 564, 291, 291, 567, 292, 292, 293, 293, 573, 574,
    216, 576, 234, 578, 51,  52,  53,  54,  55,  56,  585, 294, 294, 588, 589, 178, 591, 592, 236,
    594, 295, 295, 597, 598, 599, 600, 298, 298, 495, 496, 150, 151, 152, 237, 153, 154, 155, 612,
    238, 240, 614, 377, 378, 379, 457, 242, 277, 156, 157, 158, 460, 462, 463, 617, 159, 160, 161,
    467, 468, 472, 478, 479, 480, 482, 487, 623, 624, 625, 626, 554, 488, 490, 492, 499, 627, 628,
    212, 502, 630, 254, 503, 505, 631, 696, 633, 634, 506, 508, 210, 636, 211, 637, 535, 511, 638,
    639, 640, 213, 512, 642, 643, 513, 644, 645, 233, 646, 250, 252, 647, 648, 649, 650, 221, 222,
    514, 530, 223, 224, 526, 537, 555, 538, 540, 652, 176, 177, 178, 179, 543, 225, 226, 544, 545,
    556, 558, 559, 227, 228, 669, 669, 560, 262, 561, 674, 562, 566, 676, 568, 569, 570, 571, 575,
    684, 577, 685, 686, 687, 579, 690, 691, 692, 580, 694, 581, 582, 583, 584, 587, 148, 149, 590,
    593, 229, 595, 697, 596, 700, 150, 151, 152, 705, 153, 154, 155, 601, 632, 602, 603, 641, 604,
    713, 605, 715, 716, 156, 157, 158, 606, 651, 665, 607, 159, 160, 161, 608, 609, 610, 613, 615,
    730, 616, 618, 732, 622, 629, 162, 466, 635, 454, 663, 653, 654, 655, 473, 656, 739, 476, 477,
    662, 469, 470, 471, 658, 668, 484, 485, 277, 278, 672, 489, 673, 675, 750, 677, 678, 164, 486,
    679, 681, 682, 501, 754, 755, 504, 683, 688, 497, 498, 689, 500, 165, 166, 167, 168, 169, 170,
    171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 693, 695, 698, 701, 525,
    704, 706, 707, 714, 708, 709, 710, 711, 251, 712, 717, 718, 719, 720, 721, 731, 722, 723, 724,
    725, 726, 740, 727, 728, 729, 550, 741, 239, 735, 736, 737, 738, 742, 743, 660, 744, 745, 550,
    746, 531, 532, 533, 747, 65,  66,  67,  68,  69,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  748, 34,  35,  36,  749, 37,  38,  70,  752, 753, 71,  72,  73,  74,  75,
    76,  77,  756, 757, 758, 78,  759, 760, 761, 762, 79,  80,  81,  82,  83,  84,  40,  85,  86,
    763, 764, 376, 87,  88,  89,  90,  667, 733, 550, 91,  92,  93,  94,  95,  96,  97,  657, 98,
    99,  100, 703, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116,
    117, 118, 119, 120, 671, 455, 121, 122, 123, 124, 125, 126, 127, 0,   128, 129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 140, 43,  0,   0,   380, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   550, 65,  66,  67,  68,  69,  21,  22,  23,  24,  25,  26,  27,  28,
    29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  70,  0,   0,   71,  72,  73,  74,
    75,  76,  77,  0,   0,   0,   78,  0,   550, 0,   0,   246, 80,  81,  82,  83,  84,  247, 85,
    86,  0,   0,   0,   87,  88,  89,  90,  0,   0,   0,   91,  92,  93,  94,  95,  96,  97,  0,
    98,  99,  100, 0,   101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 118, 119, 120, 0,   0,   121, 122, 123, 124, 125, 126, 127, 0,   128, 129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 139, 140, 43,  65,  66,  67,  68,  69,  21,  22,  23,  24,
    25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,  38,  70,  0,   0,
    71,  72,  73,  74,  75,  76,  77,  0,   0,   0,   78,  0,   0,   0,   0,   659, 80,  81,  82,
    83,  84,  40,  85,  86,  0,   0,   0,   87,  88,  89,  90,  0,   0,   0,   91,  92,  93,  94,
    95,  96,  97,  0,   98,  99,  100, 0,   101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 0,   0,   121, 122, 123, 124, 125, 126, 127, 0,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 43,  385, 386, 387, 388, 389,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   390, 0,   0,   391, 392, 393, 394, 395, 396, 397, 0,   0,   0,   398, 0,   0,   0,   0,
    0,   399, 400, 401, 402, 403, 0,   404, 405, 0,   0,   0,   406, 407, 408, 409, 0,   0,   0,
    410, 411, 412, 0,   413, 414, 415, 0,   416, 417, 418, 0,   419, 420, 421, 422, 423, 424, 425,
    426, 427, 428, 429, 430, 431, 432, 0,   0,   0,   0,   0,   0,   0,   0,   433, 434, 435, 436,
    437, 438, 439, 0,   440, 441, 442, 443, 444, 445, 446, 447, 448, 449, 450, 451, 452, 20,  0,
    21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,  0,   37,
    38,  0,   21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  0,   34,  35,  36,
    39,  37,  38,  0,   0,   0,   40,  0,   0,   148, 149, 0,   0,   0,   0,   0,   0,   0,   150,
    151, 152, 528, 153, 154, 155, 0,   41,  40,  42,  0,   0,   0,   0,   0,   0,   156, 157, 158,
    0,   0,   0,   0,   159, 160, 161, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   162,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   115, 116, 117, 118, 119, 120, 0,   0,   43,  0,
    0,   547, 548, 0,   0,   0,   0,   0,   0,   0,   164, 0,   0,   0,   0,   0,   0,   0,   0,
    43,  0,   0,   0,   0,   0,   165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177,
    178, 179, 180, 181, 182, 183, 184, 148, 149, 0,   0,   0,   0,   0,   0,   0,   150, 151, 152,
    0,   153, 154, 155, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   156, 157, 158, 0,   0,
    0,   0,   159, 160, 161, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   162, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    163, 0,   0,   0,   0,   0,   0,   0,   164, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 263, 264, 0,   0,   0,   0,   0,   0,   0,   265, 266, 267, 0,   268,
    269, 270, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   271, 272, 273, 0,   0,   0,   0,
    274, 275, 276, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   162, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   277, 278, 0,
    0,   0,   0,   0,   0,   0,   164, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 279, 280, 281, 282, 180, 181,
    182};

const short ParserGen::yycheck_[] = {
    44,  16,  16,  16,  56,  468, 52,  98,  382, 55,  385, 235, 55,  3,   4,   76,  391, 480, 52,
    76,  52,  55,  45,  55,  703, 400, 401, 52,  46,  46,  55,  386, 407, 388, 409, 12,  127, 42,
    43,  44,  98,  242, 19,  20,  21,  106, 247, 248, 25,  106, 97,  500, 247, 248, 733, 73,  73,
    432, 98,  434, 435, 0,   417, 98,  121, 440, 441, 442, 443, 444, 445, 446, 447, 448, 449, 98,
    525, 452, 45,  237, 247, 248, 247, 248, 46,  460, 97,  462, 463, 247, 248, 247, 248, 467, 247,
    248, 98,  472, 247, 248, 247, 248, 98,  478, 479, 247, 248, 482, 247, 248, 247, 248, 487, 488,
    98,  490, 46,  492, 91,  92,  93,  94,  95,  96,  499, 247, 248, 502, 503, 134, 505, 506, 24,
    508, 247, 248, 511, 512, 513, 514, 247, 248, 420, 421, 38,  39,  40,  9,   42,  43,  44,  526,
    98,  68,  529, 46,  19,  46,  45,  73,  97,  55,  56,  57,  97,  97,  97,  542, 62,  63,  64,
    97,  97,  97,  97,  97,  97,  97,  97,  554, 555, 556, 557, 15,  97,  97,  97,  97,  563, 564,
    236, 97,  567, 236, 97,  97,  571, 660, 573, 574, 97,  97,  236, 578, 236, 580, 46,  97,  583,
    584, 585, 236, 97,  588, 589, 97,  591, 592, 208, 594, 235, 235, 597, 598, 599, 600, 38,  39,
    97,  453, 42,  43,  97,  46,  11,  46,  46,  612, 132, 133, 134, 135, 46,  55,  56,  46,  46,
    10,  46,  46,  62,  63,  627, 628, 46,  245, 46,  632, 46,  46,  634, 46,  46,  46,  8,   46,
    641, 46,  642, 643, 644, 46,  647, 648, 649, 15,  651, 46,  46,  13,  13,  46,  29,  30,  46,
    46,  98,  46,  663, 46,  665, 38,  39,  40,  669, 42,  43,  44,  46,  15,  46,  46,  15,  46,
    679, 46,  681, 682, 55,  56,  57,  46,  15,  14,  46,  62,  63,  64,  46,  46,  46,  46,  45,
    698, 45,  45,  701, 45,  45,  76,  392, 45,  378, 16,  46,  46,  45,  399, 46,  714, 402, 403,
    46,  395, 396, 397, 45,  45,  410, 411, 97,  98,  46,  415, 45,  45,  731, 46,  45,  106, 412,
    18,  15,  15,  426, 740, 741, 429, 45,  45,  422, 423, 45,  425, 121, 122, 123, 124, 125, 126,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 45,  45,  17,  26,  450,
    46,  45,  45,  22,  46,  46,  46,  45,  235, 46,  46,  46,  45,  45,  45,  27,  46,  46,  45,
    45,  45,  23,  46,  46,  46,  468, 23,  220, 46,  46,  46,  46,  46,  46,  621, 46,  46,  480,
    46,  453, 453, 453, 46,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  46,  22,  23,  24,  46,  26,  27,  28,  46,  46,  31,  32,  33,  34,  35,
    36,  37,  46,  46,  46,  41,  46,  46,  46,  46,  46,  47,  48,  49,  50,  51,  52,  53,  54,
    46,  46,  248, 58,  59,  60,  61,  625, 702, 547, 65,  66,  67,  68,  69,  70,  71,  619, 73,
    74,  75,  667, 77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,
    93,  94,  95,  96,  628, 380, 99,  100, 101, 102, 103, 104, 105, -1,  107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, -1,  -1,  259, -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  619, 3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,
    16,  17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,  27,  28,  -1,  -1,  31,  32,  33,  34,
    35,  36,  37,  -1,  -1,  -1,  41,  -1,  660, -1,  -1,  46,  47,  48,  49,  50,  51,  52,  53,
    54,  -1,  -1,  -1,  58,  59,  60,  61,  -1,  -1,  -1,  65,  66,  67,  68,  69,  70,  71,  -1,
    73,  74,  75,  -1,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,
    92,  93,  94,  95,  96,  -1,  -1,  99,  100, 101, 102, 103, 104, 105, -1,  107, 108, 109, 110,
    111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 3,   4,   5,   6,   7,   8,   9,   10,  11,
    12,  13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,  27,  28,  -1,  -1,
    31,  32,  33,  34,  35,  36,  37,  -1,  -1,  -1,  41,  -1,  -1,  -1,  -1,  46,  47,  48,  49,
    50,  51,  52,  53,  54,  -1,  -1,  -1,  58,  59,  60,  61,  -1,  -1,  -1,  65,  66,  67,  68,
    69,  70,  71,  -1,  73,  74,  75,  -1,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,
    88,  89,  90,  91,  92,  93,  94,  95,  96,  -1,  -1,  99,  100, 101, 102, 103, 104, 105, -1,
    107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 3,   4,   5,   6,   7,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  28,  -1,  -1,  31,  32,  33,  34,  35,  36,  37,  -1,  -1,  -1,  41,  -1,  -1,  -1,  -1,
    -1,  47,  48,  49,  50,  51,  -1,  53,  54,  -1,  -1,  -1,  58,  59,  60,  61,  -1,  -1,  -1,
    65,  66,  67,  -1,  69,  70,  71,  -1,  73,  74,  75,  -1,  77,  78,  79,  80,  81,  82,  83,
    84,  85,  86,  87,  88,  89,  90,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  99,  100, 101, 102,
    103, 104, 105, -1,  107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 6,   -1,
    8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,  -1,  26,
    27,  -1,  8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  -1,  22,  23,  24,
    46,  26,  27,  -1,  -1,  -1,  52,  -1,  -1,  29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,
    39,  40,  46,  42,  43,  44,  -1,  72,  52,  74,  -1,  -1,  -1,  -1,  -1,  -1,  55,  56,  57,
    -1,  -1,  -1,  -1,  62,  63,  64,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  76,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  91,  92,  93,  94,  95,  96,  -1,  -1,  120, -1,
    -1,  97,  98,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    120, -1,  -1,  -1,  -1,  -1,  121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133,
    134, 135, 136, 137, 138, 139, 140, 29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,  39,  40,
    -1,  42,  43,  44,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  55,  56,  57,  -1,  -1,
    -1,  -1,  62,  63,  64,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  76,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    98,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 137, 138, 139, 140, 29,  30,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  38,  39,  40,  -1,  42,
    43,  44,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  55,  56,  57,  -1,  -1,  -1,  -1,
    62,  63,  64,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  76,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  97,  98,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  106, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
    138};

const short ParserGen::yystos_[] = {
    0,   12,  19,  20,  21,  25,  301, 98,  285, 97,  182, 285, 285, 98,  295, 0,   286, 98,  183,
    296, 6,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  22,  23,  24,  26,
    27,  46,  52,  72,  74,  120, 146, 147, 149, 150, 290, 291, 294, 91,  92,  93,  94,  95,  96,
    184, 185, 186, 187, 188, 189, 190, 45,  3,   4,   5,   6,   7,   28,  31,  32,  33,  34,  35,
    36,  37,  41,  46,  47,  48,  49,  50,  51,  53,  54,  58,  59,  60,  61,  65,  66,  67,  68,
    69,  70,  71,  73,  74,  75,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  99,  100, 101, 102, 103, 104, 105, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 145, 147, 148, 149, 150, 151, 300, 29,  30,  38,  39,
    40,  42,  43,  44,  55,  56,  57,  62,  63,  64,  76,  98,  106, 121, 122, 123, 124, 125, 126,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 181, 288, 97,
    98,  159, 161, 162, 163, 193, 98,  98,  193, 302, 303, 46,  38,  39,  42,  43,  55,  56,  62,
    63,  98,  297, 298, 287, 285, 46,  191, 24,  9,   98,  183, 68,  46,  73,  292, 293, 289, 46,
    52,  143, 145, 147, 148, 149, 152, 193, 167, 76,  106, 299, 98,  175, 45,  285, 29,  30,  38,
    39,  40,  42,  43,  44,  55,  56,  57,  62,  63,  64,  97,  98,  132, 133, 134, 135, 155, 156,
    157, 158, 160, 164, 165, 167, 169, 170, 171, 173, 174, 175, 192, 195, 197, 198, 200, 201, 202,
    203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 221, 222,
    223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 260,
    261, 262, 263, 264, 265, 266, 267, 277, 278, 279, 280, 281, 282, 283, 284, 192, 46,  19,  46,
    292, 176, 194, 195, 274, 3,   4,   5,   6,   7,   28,  31,  32,  33,  34,  35,  36,  37,  41,
    47,  48,  49,  50,  51,  53,  54,  58,  59,  60,  61,  65,  66,  67,  69,  70,  71,  73,  74,
    75,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  99,  100, 101, 102,
    103, 104, 105, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 199, 162, 287,
    274, 45,  194, 197, 97,  197, 97,  97,  196, 194, 196, 97,  97,  302, 302, 302, 97,  196, 194,
    194, 196, 196, 97,  97,  97,  194, 97,  194, 196, 196, 302, 97,  97,  196, 97,  197, 97,  220,
    302, 220, 220, 302, 302, 97,  302, 196, 97,  97,  196, 97,  97,  194, 97,  194, 194, 97,  97,
    97,  97,  194, 194, 194, 194, 194, 194, 194, 194, 194, 194, 302, 97,  194, 46,  144, 145, 147,
    149, 150, 153, 46,  46,  46,  46,  194, 46,  194, 194, 46,  46,  46,  274, 97,  98,  166, 176,
    177, 178, 179, 15,  11,  10,  194, 46,  46,  46,  46,  46,  194, 194, 166, 46,  194, 46,  46,
    46,  8,   272, 194, 194, 46,  194, 46,  194, 46,  15,  46,  46,  13,  13,  194, 272, 46,  194,
    194, 46,  194, 194, 46,  194, 46,  46,  194, 194, 194, 194, 46,  46,  46,  46,  46,  46,  46,
    46,  46,  46,  272, 194, 46,  194, 45,  45,  194, 45,  166, 275, 180, 45,  194, 194, 194, 194,
    194, 194, 45,  194, 194, 15,  194, 194, 45,  194, 194, 194, 194, 194, 15,  194, 194, 194, 194,
    194, 194, 194, 194, 194, 15,  194, 46,  46,  45,  46,  275, 45,  46,  151, 154, 46,  16,  268,
    14,  270, 270, 45,  194, 276, 276, 46,  45,  194, 45,  274, 46,  45,  18,  273, 15,  15,  45,
    194, 274, 274, 274, 45,  45,  194, 194, 194, 45,  194, 45,  166, 194, 17,  269, 194, 26,  271,
    271, 46,  194, 45,  45,  46,  46,  46,  45,  46,  194, 22,  194, 194, 46,  46,  45,  45,  45,
    46,  46,  45,  45,  45,  46,  46,  46,  194, 27,  194, 268, 269, 46,  46,  46,  46,  194, 23,
    23,  46,  46,  46,  46,  46,  46,  46,  46,  194, 269, 46,  46,  194, 194, 46,  46,  46,  46,
    46,  46,  46,  46,  46};

const short ParserGen::yyr1_[] = {
    0,   142, 301, 301, 301, 301, 301, 182, 183, 183, 303, 302, 184, 184, 184, 184, 184, 184, 190,
    185, 186, 193, 193, 193, 193, 187, 188, 189, 191, 191, 152, 152, 192, 192, 192, 192, 192, 192,
    192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192,
    192, 192, 192, 192, 192, 192, 192, 192, 143, 143, 143, 143, 285, 286, 286, 290, 290, 288, 288,
    287, 287, 292, 293, 293, 291, 294, 294, 294, 289, 289, 146, 146, 146, 149, 145, 145, 145, 145,
    145, 145, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147, 147,
    147, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148,
    148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148,
    148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148,
    148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 148, 167, 167, 167, 168, 181, 169,
    170, 171, 173, 174, 175, 155, 156, 157, 158, 160, 164, 165, 159, 159, 159, 159, 161, 161, 161,
    161, 162, 162, 162, 162, 163, 163, 163, 163, 172, 172, 176, 176, 176, 176, 176, 176, 176, 176,
    176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 176, 274, 274, 194, 194, 196, 195,
    195, 195, 195, 195, 195, 195, 195, 195, 197, 198, 199, 199, 153, 144, 144, 144, 144, 150, 200,
    200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 201, 202, 253,
    254, 255, 256, 257, 258, 259, 260, 261, 262, 263, 264, 265, 266, 267, 203, 203, 203, 204, 205,
    206, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210, 210,
    210, 210, 210, 210, 211, 270, 270, 271, 271, 212, 213, 276, 276, 276, 214, 215, 272, 272, 216,
    223, 233, 273, 273, 220, 217, 218, 219, 221, 222, 224, 225, 226, 227, 228, 229, 230, 231, 232,
    299, 299, 297, 295, 296, 296, 298, 298, 298, 298, 298, 298, 298, 298, 300, 300, 277, 277, 277,
    277, 277, 277, 277, 278, 279, 280, 281, 282, 283, 284, 207, 207, 208, 209, 166, 166, 177, 177,
    178, 275, 275, 179, 180, 180, 154, 151, 151, 151, 151, 151, 234, 234, 234, 234, 234, 234, 234,
    235, 236, 237, 238, 239, 240, 241, 242, 242, 242, 242, 242, 242, 242, 242, 242, 242, 268, 268,
    269, 269, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252};

const signed char ParserGen::yyr2_[] = {
    0, 2, 2,  2,  2, 2, 2, 3, 0, 4, 0, 2, 1, 1, 1, 1, 1, 1, 5, 3,  7,  1,  1, 1, 1, 2, 2, 4, 0, 2,
    2, 2, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 3, 0, 2, 2, 1, 1, 3, 0, 2, 1, 2,  5,  5,  1, 1, 1, 0, 2, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 0, 2,  1,  1, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2,  1,  1,  1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1,  1,  1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4,  7,  4,  7, 8, 7, 7, 4, 7, 7, 1,
    1, 1, 4,  4,  6, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 6, 0, 2,
    0, 2, 11, 10, 0, 1, 2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4,  11, 11, 7, 4, 4, 7, 8, 8, 8, 4,
    4, 1, 1,  4,  3, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1,  1,  1,  1, 1, 6, 6, 4, 8, 8, 4,
    8, 1, 1,  6,  6, 1, 1, 1, 1, 3, 0, 2, 3, 0, 2, 2, 1, 1, 1, 1,  1,  1,  1, 1, 1, 1, 1, 1, 4, 4,
    4, 4, 4,  4,  4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 2, 0, 2, 11, 4,  4,  4, 4, 4, 4, 4, 4, 4};


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
                                           "GT",
                                           "GTE",
                                           "ID",
                                           "INDEX_OF_BYTES",
                                           "INDEX_OF_CP",
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
                                           "REGEX_FIND",
                                           "REGEX_FIND_ALL",
                                           "REGEX_MATCH",
                                           "REPLACE_ALL",
                                           "REPLACE_ONE",
                                           "ROUND",
                                           "RTRIM",
                                           "\"setDifference\"",
                                           "\"setEquals\"",
                                           "\"setIntersection\"",
                                           "\"setIsSubset\"",
                                           "\"setUnion\"",
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
                                           "\"string\"",
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
                                           "\"$-prefixed string\"",
                                           "\"$$-prefixed string\"",
                                           "\"$-prefixed fieldname\"",
                                           "$accept",
                                           "projectionFieldname",
                                           "expressionFieldname",
                                           "stageAsUserFieldname",
                                           "predFieldname",
                                           "argAsUserFieldname",
                                           "aggExprAsUserFieldname",
                                           "invariableUserFieldname",
                                           "idAsUserFieldname",
                                           "valueFieldname",
                                           "projectField",
                                           "expressionField",
                                           "valueField",
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
                                           "fieldPath",
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
                                           "projection",
                                           "num",
                                           "expression",
                                           "compoundExpression",
                                           "exprFixedTwoArg",
                                           "expressionArray",
                                           "expressionObject",
                                           "expressionFields",
                                           "maths",
                                           "add",
                                           "atan2",
                                           "boolExps",
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
    0,    313,  313,  316,  319,  322,  325,  332,  338,  339,  347,  347,  350,  350,  350,  350,
    350,  350,  353,  363,  369,  379,  379,  379,  379,  383,  388,  393,  409,  412,  419,  422,
    428,  429,  430,  431,  432,  433,  434,  435,  436,  437,  438,  439,  442,  445,  448,  451,
    454,  457,  460,  463,  466,  469,  472,  475,  478,  481,  484,  487,  490,  493,  494,  495,
    496,  505,  505,  505,  505,  509,  515,  518,  524,  527,  536,  537,  543,  546,  553,  556,
    560,  569,  577,  578,  579,  582,  585,  592,  592,  592,  595,  603,  606,  609,  612,  615,
    618,  627,  630,  633,  636,  639,  642,  645,  648,  651,  654,  657,  660,  663,  666,  669,
    672,  675,  678,  686,  689,  692,  695,  698,  701,  704,  707,  710,  713,  716,  719,  722,
    725,  728,  731,  734,  737,  740,  743,  746,  749,  752,  755,  758,  761,  764,  767,  770,
    773,  776,  779,  782,  785,  788,  791,  794,  797,  800,  803,  806,  809,  812,  815,  818,
    821,  824,  827,  830,  833,  836,  839,  842,  845,  848,  851,  854,  857,  860,  863,  866,
    869,  872,  875,  878,  881,  884,  887,  890,  897,  902,  905,  911,  919,  928,  934,  940,
    946,  952,  958,  964,  970,  976,  982,  988,  994,  1000, 1006, 1009, 1012, 1015, 1021, 1024,
    1027, 1030, 1036, 1039, 1042, 1045, 1051, 1054, 1057, 1060, 1066, 1069, 1075, 1076, 1077, 1078,
    1079, 1080, 1081, 1082, 1083, 1084, 1085, 1086, 1087, 1088, 1089, 1090, 1091, 1092, 1093, 1094,
    1095, 1102, 1103, 1110, 1110, 1114, 1119, 1119, 1119, 1119, 1119, 1119, 1120, 1120, 1120, 1126,
    1134, 1140, 1143, 1150, 1157, 1157, 1157, 1157, 1161, 1167, 1167, 1167, 1167, 1167, 1167, 1167,
    1167, 1167, 1167, 1167, 1167, 1167, 1168, 1168, 1168, 1168, 1172, 1179, 1185, 1190, 1195, 1201,
    1206, 1211, 1216, 1222, 1227, 1233, 1242, 1248, 1254, 1259, 1265, 1271, 1271, 1271, 1275, 1282,
    1289, 1296, 1296, 1296, 1296, 1296, 1296, 1296, 1297, 1297, 1297, 1297, 1297, 1297, 1297, 1297,
    1298, 1298, 1298, 1298, 1298, 1298, 1298, 1302, 1312, 1315, 1321, 1324, 1330, 1339, 1348, 1351,
    1354, 1360, 1371, 1382, 1385, 1391, 1399, 1407, 1415, 1418, 1423, 1432, 1438, 1444, 1450, 1460,
    1470, 1477, 1484, 1491, 1499, 1507, 1515, 1523, 1529, 1535, 1538, 1544, 1550, 1555, 1558, 1565,
    1568, 1571, 1574, 1577, 1580, 1583, 1586, 1591, 1593, 1599, 1599, 1599, 1599, 1599, 1599, 1600,
    1604, 1610, 1616, 1623, 1634, 1645, 1652, 1663, 1663, 1667, 1674, 1681, 1681, 1685, 1685, 1689,
    1695, 1696, 1703, 1709, 1712, 1719, 1726, 1727, 1728, 1729, 1730, 1733, 1733, 1733, 1733, 1733,
    1733, 1733, 1735, 1740, 1745, 1750, 1755, 1760, 1765, 1771, 1772, 1773, 1774, 1775, 1776, 1777,
    1778, 1779, 1780, 1785, 1788, 1795, 1798, 1804, 1814, 1819, 1824, 1829, 1834, 1839, 1844, 1849,
    1854};

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
#line 6344 "parser_gen.cpp"

#line 1858 "grammar.yy"
