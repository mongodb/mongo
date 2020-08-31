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


#include "pipeline_parser_gen.hpp"


// Unqualified %code blocks.
#line 83 "pipeline_grammar.yy"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node_disambiguation.h"
#include "mongo/db/cst/c_node_validation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"

namespace mongo {
// Mandatory error function.
void PipelineParserGen::error(const PipelineParserGen::location_type& loc, const std::string& msg) {
    uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
}
}  // namespace mongo

// Default location for actions, called each time a rule is matched but before the action is
// run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
// mongo.
#define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)

#line 68 "pipeline_parser_gen.cpp"


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

#line 58 "pipeline_grammar.yy"
namespace mongo {
#line 161 "pipeline_parser_gen.cpp"

/// Build a parser object.
PipelineParserGen::PipelineParserGen(BSONLexer& lexer_yyarg, CNode* cst_yyarg)
#if YYDEBUG
    : yydebug_(false),
      yycdebug_(&std::cerr),
#else
    :
#endif
      lexer(lexer_yyarg),
      cst(cst_yyarg) {
}

PipelineParserGen::~PipelineParserGen() {}

PipelineParserGen::syntax_error::~syntax_error() YY_NOEXCEPT YY_NOTHROW {}

/*---------------.
| symbol kinds.  |
`---------------*/


// by_state.
PipelineParserGen::by_state::by_state() YY_NOEXCEPT : state(empty_state) {}

PipelineParserGen::by_state::by_state(const by_state& that) YY_NOEXCEPT : state(that.state) {}

void PipelineParserGen::by_state::clear() YY_NOEXCEPT {
    state = empty_state;
}

void PipelineParserGen::by_state::move(by_state& that) {
    state = that.state;
    that.clear();
}

PipelineParserGen::by_state::by_state(state_type s) YY_NOEXCEPT : state(s) {}

PipelineParserGen::symbol_kind_type PipelineParserGen::by_state::kind() const YY_NOEXCEPT {
    if (state == empty_state)
        return symbol_kind::S_YYEMPTY;
    else
        return YY_CAST(symbol_kind_type, yystos_[+state]);
}

PipelineParserGen::stack_symbol_type::stack_symbol_type() {}

PipelineParserGen::stack_symbol_type::stack_symbol_type(YY_RVREF(stack_symbol_type) that)
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
        case symbol_kind::S_matchExpression:      // matchExpression
        case symbol_kind::S_filterFields:         // filterFields
        case symbol_kind::S_filterVal:            // filterVal
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
        case symbol_kind::S_filterFieldname:          // filterFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
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

        case symbol_kind::S_projectField:     // projectField
        case symbol_kind::S_expressionField:  // expressionField
        case symbol_kind::S_valueField:       // valueField
        case symbol_kind::S_filterField:      // filterField
        case symbol_kind::S_onErrorArg:       // onErrorArg
        case symbol_kind::S_onNullArg:        // onNullArg
        case symbol_kind::S_formatArg:        // formatArg
        case symbol_kind::S_timezoneArg:      // timezoneArg
        case symbol_kind::S_charsArg:         // charsArg
        case symbol_kind::S_optionsArg:       // optionsArg
        case symbol_kind::S_sortSpec:         // sortSpec
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

PipelineParserGen::stack_symbol_type::stack_symbol_type(state_type s, YY_MOVE_REF(symbol_type) that)
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
        case symbol_kind::S_matchExpression:      // matchExpression
        case symbol_kind::S_filterFields:         // filterFields
        case symbol_kind::S_filterVal:            // filterVal
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
        case symbol_kind::S_filterFieldname:          // filterFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
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

        case symbol_kind::S_projectField:     // projectField
        case symbol_kind::S_expressionField:  // expressionField
        case symbol_kind::S_valueField:       // valueField
        case symbol_kind::S_filterField:      // filterField
        case symbol_kind::S_onErrorArg:       // onErrorArg
        case symbol_kind::S_onNullArg:        // onNullArg
        case symbol_kind::S_formatArg:        // formatArg
        case symbol_kind::S_timezoneArg:      // timezoneArg
        case symbol_kind::S_charsArg:         // charsArg
        case symbol_kind::S_optionsArg:       // optionsArg
        case symbol_kind::S_sortSpec:         // sortSpec
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
PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
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
        case symbol_kind::S_matchExpression:      // matchExpression
        case symbol_kind::S_filterFields:         // filterFields
        case symbol_kind::S_filterVal:            // filterVal
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
        case symbol_kind::S_filterFieldname:          // filterFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
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

        case symbol_kind::S_projectField:     // projectField
        case symbol_kind::S_expressionField:  // expressionField
        case symbol_kind::S_valueField:       // valueField
        case symbol_kind::S_filterField:      // filterField
        case symbol_kind::S_onErrorArg:       // onErrorArg
        case symbol_kind::S_onNullArg:        // onNullArg
        case symbol_kind::S_formatArg:        // formatArg
        case symbol_kind::S_timezoneArg:      // timezoneArg
        case symbol_kind::S_charsArg:         // charsArg
        case symbol_kind::S_optionsArg:       // optionsArg
        case symbol_kind::S_sortSpec:         // sortSpec
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

PipelineParserGen::stack_symbol_type& PipelineParserGen::stack_symbol_type::operator=(
    stack_symbol_type& that) {
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
        case symbol_kind::S_matchExpression:      // matchExpression
        case symbol_kind::S_filterFields:         // filterFields
        case symbol_kind::S_filterVal:            // filterVal
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
        case symbol_kind::S_filterFieldname:          // filterFieldname
        case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
        case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
        case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
        case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
        case symbol_kind::S_valueFieldname:           // valueFieldname
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

        case symbol_kind::S_projectField:     // projectField
        case symbol_kind::S_expressionField:  // expressionField
        case symbol_kind::S_valueField:       // valueField
        case symbol_kind::S_filterField:      // filterField
        case symbol_kind::S_onErrorArg:       // onErrorArg
        case symbol_kind::S_onNullArg:        // onNullArg
        case symbol_kind::S_formatArg:        // formatArg
        case symbol_kind::S_timezoneArg:      // timezoneArg
        case symbol_kind::S_charsArg:         // charsArg
        case symbol_kind::S_optionsArg:       // optionsArg
        case symbol_kind::S_sortSpec:         // sortSpec
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
void PipelineParserGen::yy_destroy_(const char* yymsg, basic_symbol<Base>& yysym) const {
    if (yymsg)
        YY_SYMBOL_PRINT(yymsg, yysym);
}

#if YYDEBUG
template <typename Base>
void PipelineParserGen::yy_print_(std::ostream& yyo, const basic_symbol<Base>& yysym) const {
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

void PipelineParserGen::yypush_(const char* m, YY_MOVE_REF(stack_symbol_type) sym) {
    if (m)
        YY_SYMBOL_PRINT(m, sym);
    yystack_.push(YY_MOVE(sym));
}

void PipelineParserGen::yypush_(const char* m, state_type s, YY_MOVE_REF(symbol_type) sym) {
#if 201103L <= YY_CPLUSPLUS
    yypush_(m, stack_symbol_type(s, std::move(sym)));
#else
    stack_symbol_type ss(s, sym);
    yypush_(m, ss);
#endif
}

void PipelineParserGen::yypop_(int n) {
    yystack_.pop(n);
}

#if YYDEBUG
std::ostream& PipelineParserGen::debug_stream() const {
    return *yycdebug_;
}

void PipelineParserGen::set_debug_stream(std::ostream& o) {
    yycdebug_ = &o;
}


PipelineParserGen::debug_level_type PipelineParserGen::debug_level() const {
    return yydebug_;
}

void PipelineParserGen::set_debug_level(debug_level_type l) {
    yydebug_ = l;
}
#endif  // YYDEBUG

PipelineParserGen::state_type PipelineParserGen::yy_lr_goto_state_(state_type yystate, int yysym) {
    int yyr = yypgoto_[yysym - YYNTOKENS] + yystate;
    if (0 <= yyr && yyr <= yylast_ && yycheck_[yyr] == yystate)
        return yytable_[yyr];
    else
        return yydefgoto_[yysym - YYNTOKENS];
}

bool PipelineParserGen::yy_pact_value_is_default_(int yyvalue) {
    return yyvalue == yypact_ninf_;
}

bool PipelineParserGen::yy_table_value_is_error_(int yyvalue) {
    return yyvalue == yytable_ninf_;
}

int PipelineParserGen::operator()() {
    return parse();
}

int PipelineParserGen::parse() {
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
                case symbol_kind::S_matchExpression:      // matchExpression
                case symbol_kind::S_filterFields:         // filterFields
                case symbol_kind::S_filterVal:            // filterVal
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
                case symbol_kind::S_filterFieldname:          // filterFieldname
                case symbol_kind::S_argAsUserFieldname:       // argAsUserFieldname
                case symbol_kind::S_aggExprAsUserFieldname:   // aggExprAsUserFieldname
                case symbol_kind::S_invariableUserFieldname:  // invariableUserFieldname
                case symbol_kind::S_idAsUserFieldname:        // idAsUserFieldname
                case symbol_kind::S_valueFieldname:           // valueFieldname
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

                case symbol_kind::S_projectField:     // projectField
                case symbol_kind::S_expressionField:  // expressionField
                case symbol_kind::S_valueField:       // valueField
                case symbol_kind::S_filterField:      // filterField
                case symbol_kind::S_onErrorArg:       // onErrorArg
                case symbol_kind::S_onNullArg:        // onNullArg
                case symbol_kind::S_formatArg:        // formatArg
                case symbol_kind::S_timezoneArg:      // timezoneArg
                case symbol_kind::S_charsArg:         // charsArg
                case symbol_kind::S_optionsArg:       // optionsArg
                case symbol_kind::S_sortSpec:         // sortSpec
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
                    case 2:  // start: START_PIPELINE pipeline
#line 297 "pipeline_grammar.yy"
                    {
                        invariant(cst);
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1719 "pipeline_parser_gen.cpp"
                    break;

                    case 3:  // start: START_MATCH matchExpression
#line 301 "pipeline_grammar.yy"
                    {
                        invariant(cst);
                        *cst = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1728 "pipeline_parser_gen.cpp"
                    break;

                    case 4:  // start: START_SORT sortSpecs
#line 305 "pipeline_grammar.yy"
                    {
                        *cst = CNode{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1736 "pipeline_parser_gen.cpp"
                    break;

                    case 5:  // pipeline: "array" stageList "end of array"
#line 312 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 1744 "pipeline_parser_gen.cpp"
                    break;

                    case 6:  // stageList: %empty
#line 318 "pipeline_grammar.yy"
                    {
                    }
#line 1750 "pipeline_parser_gen.cpp"
                    break;

                    case 7:  // stageList: "object" stage "end of object" stageList
#line 319 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}};
                    }
#line 1758 "pipeline_parser_gen.cpp"
                    break;

                    case 8:  // $@1: %empty
#line 327 "pipeline_grammar.yy"
                    {
                        lexer.sortObjTokens();
                    }
#line 1764 "pipeline_parser_gen.cpp"
                    break;

                    case 10:  // stage: inhibitOptimization
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1770 "pipeline_parser_gen.cpp"
                    break;

                    case 11:  // stage: unionWith
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1776 "pipeline_parser_gen.cpp"
                    break;

                    case 12:  // stage: skip
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1782 "pipeline_parser_gen.cpp"
                    break;

                    case 13:  // stage: limit
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1788 "pipeline_parser_gen.cpp"
                    break;

                    case 14:  // stage: project
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1794 "pipeline_parser_gen.cpp"
                    break;

                    case 15:  // stage: sample
#line 330 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1800 "pipeline_parser_gen.cpp"
                    break;

                    case 16:  // sample: STAGE_SAMPLE "object" "size argument" num "end of object"
#line 333 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::sample,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::sizeArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            }}}}};
                    }
#line 1812 "pipeline_parser_gen.cpp"
                    break;

                    case 17:  // inhibitOptimization: STAGE_INHIBIT_OPTIMIZATION "object" "end of
                              // object"
#line 343 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
                    }
#line 1820 "pipeline_parser_gen.cpp"
                    break;

                    case 18:  // unionWith: STAGE_UNION_WITH START_ORDERED_OBJECT "coll argument"
                              // string "pipeline argument" double "end of object"
#line 349 "pipeline_grammar.yy"
                    {
                        auto pipeline = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::unionWith,
                            CNode{CNode::ObjectChildren{
                                {KeyFieldname::collArg, YY_MOVE(yystack_[3].value.as<CNode>())},
                                {KeyFieldname::pipelineArg, std::move(pipeline)}}}}}};
                    }
#line 1833 "pipeline_parser_gen.cpp"
                    break;

                    case 19:  // num: int
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1839 "pipeline_parser_gen.cpp"
                    break;

                    case 20:  // num: long
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1845 "pipeline_parser_gen.cpp"
                    break;

                    case 21:  // num: double
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1851 "pipeline_parser_gen.cpp"
                    break;

                    case 22:  // num: decimal
#line 359 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1857 "pipeline_parser_gen.cpp"
                    break;

                    case 23:  // skip: STAGE_SKIP num
#line 363 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            std::pair{KeyFieldname::skip, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1865 "pipeline_parser_gen.cpp"
                    break;

                    case 24:  // limit: STAGE_LIMIT num
#line 368 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{std::pair{
                            KeyFieldname::limit, YY_MOVE(yystack_[0].value.as<CNode>())}}};
                    }
#line 1873 "pipeline_parser_gen.cpp"
                    break;

                    case 25:  // project: STAGE_PROJECT "object" projectFields "end of object"
#line 373 "pipeline_grammar.yy"
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
#line 1891 "pipeline_parser_gen.cpp"
                    break;

                    case 26:  // projectFields: %empty
#line 389 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 1899 "pipeline_parser_gen.cpp"
                    break;

                    case 27:  // projectFields: projectFields projectField
#line 392 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 1908 "pipeline_parser_gen.cpp"
                    break;

                    case 28:  // projectField: ID projection
#line 399 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            KeyFieldname::id, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1916 "pipeline_parser_gen.cpp"
                    break;

                    case 29:  // projectField: projectionFieldname projection
#line 402 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 1924 "pipeline_parser_gen.cpp"
                    break;

                    case 30:  // projection: string
#line 408 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1930 "pipeline_parser_gen.cpp"
                    break;

                    case 31:  // projection: binary
#line 409 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1936 "pipeline_parser_gen.cpp"
                    break;

                    case 32:  // projection: undefined
#line 410 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1942 "pipeline_parser_gen.cpp"
                    break;

                    case 33:  // projection: objectId
#line 411 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1948 "pipeline_parser_gen.cpp"
                    break;

                    case 34:  // projection: date
#line 412 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1954 "pipeline_parser_gen.cpp"
                    break;

                    case 35:  // projection: null
#line 413 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1960 "pipeline_parser_gen.cpp"
                    break;

                    case 36:  // projection: regex
#line 414 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1966 "pipeline_parser_gen.cpp"
                    break;

                    case 37:  // projection: dbPointer
#line 415 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1972 "pipeline_parser_gen.cpp"
                    break;

                    case 38:  // projection: javascript
#line 416 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1978 "pipeline_parser_gen.cpp"
                    break;

                    case 39:  // projection: symbol
#line 417 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1984 "pipeline_parser_gen.cpp"
                    break;

                    case 40:  // projection: javascriptWScope
#line 418 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 1990 "pipeline_parser_gen.cpp"
                    break;

                    case 41:  // projection: "1 (int)"
#line 419 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1}};
                    }
#line 1998 "pipeline_parser_gen.cpp"
                    break;

                    case 42:  // projection: "-1 (int)"
#line 422 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1}};
                    }
#line 2006 "pipeline_parser_gen.cpp"
                    break;

                    case 43:  // projection: "arbitrary integer"
#line 425 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 2014 "pipeline_parser_gen.cpp"
                    break;

                    case 44:  // projection: "zero (int)"
#line 428 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intZeroKey};
                    }
#line 2022 "pipeline_parser_gen.cpp"
                    break;

                    case 45:  // projection: "1 (long)"
#line 431 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1ll}};
                    }
#line 2030 "pipeline_parser_gen.cpp"
                    break;

                    case 46:  // projection: "-1 (long)"
#line 434 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1ll}};
                    }
#line 2038 "pipeline_parser_gen.cpp"
                    break;

                    case 47:  // projection: "arbitrary long"
#line 437 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 2046 "pipeline_parser_gen.cpp"
                    break;

                    case 48:  // projection: "zero (long)"
#line 440 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longZeroKey};
                    }
#line 2054 "pipeline_parser_gen.cpp"
                    break;

                    case 49:  // projection: "1 (double)"
#line 443 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2062 "pipeline_parser_gen.cpp"
                    break;

                    case 50:  // projection: "-1 (double)"
#line 446 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2070 "pipeline_parser_gen.cpp"
                    break;

                    case 51:  // projection: "arbitrary double"
#line 449 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 2078 "pipeline_parser_gen.cpp"
                    break;

                    case 52:  // projection: "zero (double)"
#line 452 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleZeroKey};
                    }
#line 2086 "pipeline_parser_gen.cpp"
                    break;

                    case 53:  // projection: "1 (decimal)"
#line 455 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{1.0}};
                    }
#line 2094 "pipeline_parser_gen.cpp"
                    break;

                    case 54:  // projection: "-1 (decimal)"
#line 458 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{NonZeroKey{-1.0}};
                    }
#line 2102 "pipeline_parser_gen.cpp"
                    break;

                    case 55:  // projection: "arbitrary decimal"
#line 461 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{NonZeroKey{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 2110 "pipeline_parser_gen.cpp"
                    break;

                    case 56:  // projection: "zero (decimal)"
#line 464 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalZeroKey};
                    }
#line 2118 "pipeline_parser_gen.cpp"
                    break;

                    case 57:  // projection: "true"
#line 467 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::trueKey};
                    }
#line 2126 "pipeline_parser_gen.cpp"
                    break;

                    case 58:  // projection: "false"
#line 470 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::falseKey};
                    }
#line 2134 "pipeline_parser_gen.cpp"
                    break;

                    case 59:  // projection: timestamp
#line 473 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2140 "pipeline_parser_gen.cpp"
                    break;

                    case 60:  // projection: minKey
#line 474 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2146 "pipeline_parser_gen.cpp"
                    break;

                    case 61:  // projection: maxKey
#line 475 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2152 "pipeline_parser_gen.cpp"
                    break;

                    case 62:  // projection: compoundExpression
#line 476 "pipeline_grammar.yy"
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
#line 2163 "pipeline_parser_gen.cpp"
                    break;

                    case 63:  // projectionFieldname: invariableUserFieldname
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2169 "pipeline_parser_gen.cpp"
                    break;

                    case 64:  // projectionFieldname: stageAsUserFieldname
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2175 "pipeline_parser_gen.cpp"
                    break;

                    case 65:  // projectionFieldname: argAsUserFieldname
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2181 "pipeline_parser_gen.cpp"
                    break;

                    case 66:  // projectionFieldname: aggExprAsUserFieldname
#line 485 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2187 "pipeline_parser_gen.cpp"
                    break;

                    case 67:  // matchExpression: "object" filterFields "end of object"
#line 489 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 2195 "pipeline_parser_gen.cpp"
                    break;

                    case 68:  // filterFields: %empty
#line 495 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 2203 "pipeline_parser_gen.cpp"
                    break;

                    case 69:  // filterFields: filterFields filterField
#line 498 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 2212 "pipeline_parser_gen.cpp"
                    break;

                    case 70:  // filterField: filterFieldname filterVal
#line 504 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 2220 "pipeline_parser_gen.cpp"
                    break;

                    case 71:  // filterVal: value
#line 510 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 2226 "pipeline_parser_gen.cpp"
                    break;

                    case 72:  // filterFieldname: idAsUserFieldname
#line 515 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2232 "pipeline_parser_gen.cpp"
                    break;

                    case 73:  // filterFieldname: invariableUserFieldname
#line 515 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2238 "pipeline_parser_gen.cpp"
                    break;

                    case 74:  // filterFieldname: argAsUserFieldname
#line 515 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 2244 "pipeline_parser_gen.cpp"
                    break;

                    case 75:  // invariableUserFieldname: "fieldname"
#line 519 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{YY_MOVE(yystack_[0].value.as<std::string>())};
                    }
#line 2252 "pipeline_parser_gen.cpp"
                    break;

                    case 76:  // stageAsUserFieldname: STAGE_INHIBIT_OPTIMIZATION
#line 527 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            UserFieldname{"$_internalInhibitOptimization"};
                    }
#line 2260 "pipeline_parser_gen.cpp"
                    break;

                    case 77:  // stageAsUserFieldname: STAGE_UNION_WITH
#line 530 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$unionWith"};
                    }
#line 2268 "pipeline_parser_gen.cpp"
                    break;

                    case 78:  // stageAsUserFieldname: STAGE_SKIP
#line 533 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$skip"};
                    }
#line 2276 "pipeline_parser_gen.cpp"
                    break;

                    case 79:  // stageAsUserFieldname: STAGE_LIMIT
#line 536 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$limit"};
                    }
#line 2284 "pipeline_parser_gen.cpp"
                    break;

                    case 80:  // stageAsUserFieldname: STAGE_PROJECT
#line 539 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$project"};
                    }
#line 2292 "pipeline_parser_gen.cpp"
                    break;

                    case 81:  // stageAsUserFieldname: STAGE_SAMPLE
#line 542 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sample"};
                    }
#line 2300 "pipeline_parser_gen.cpp"
                    break;

                    case 82:  // argAsUserFieldname: "coll argument"
#line 551 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"coll"};
                    }
#line 2308 "pipeline_parser_gen.cpp"
                    break;

                    case 83:  // argAsUserFieldname: "pipeline argument"
#line 554 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"pipeline"};
                    }
#line 2316 "pipeline_parser_gen.cpp"
                    break;

                    case 84:  // argAsUserFieldname: "size argument"
#line 557 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"size"};
                    }
#line 2324 "pipeline_parser_gen.cpp"
                    break;

                    case 85:  // argAsUserFieldname: "input argument"
#line 560 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"input"};
                    }
#line 2332 "pipeline_parser_gen.cpp"
                    break;

                    case 86:  // argAsUserFieldname: "to argument"
#line 563 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"to"};
                    }
#line 2340 "pipeline_parser_gen.cpp"
                    break;

                    case 87:  // argAsUserFieldname: "onError argument"
#line 566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onError"};
                    }
#line 2348 "pipeline_parser_gen.cpp"
                    break;

                    case 88:  // argAsUserFieldname: "onNull argument"
#line 569 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"onNull"};
                    }
#line 2356 "pipeline_parser_gen.cpp"
                    break;

                    case 89:  // argAsUserFieldname: "dateString argument"
#line 572 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"dateString"};
                    }
#line 2364 "pipeline_parser_gen.cpp"
                    break;

                    case 90:  // argAsUserFieldname: "format argument"
#line 575 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"format"};
                    }
#line 2372 "pipeline_parser_gen.cpp"
                    break;

                    case 91:  // argAsUserFieldname: "timezone argument"
#line 578 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"timezone"};
                    }
#line 2380 "pipeline_parser_gen.cpp"
                    break;

                    case 92:  // argAsUserFieldname: "date argument"
#line 581 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"date"};
                    }
#line 2388 "pipeline_parser_gen.cpp"
                    break;

                    case 93:  // argAsUserFieldname: "chars argument"
#line 584 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"chars"};
                    }
#line 2396 "pipeline_parser_gen.cpp"
                    break;

                    case 94:  // argAsUserFieldname: "regex argument"
#line 587 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"regex"};
                    }
#line 2404 "pipeline_parser_gen.cpp"
                    break;

                    case 95:  // argAsUserFieldname: "options argument"
#line 590 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"options"};
                    }
#line 2412 "pipeline_parser_gen.cpp"
                    break;

                    case 96:  // argAsUserFieldname: "find argument"
#line 593 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"find"};
                    }
#line 2420 "pipeline_parser_gen.cpp"
                    break;

                    case 97:  // argAsUserFieldname: "replacement argument"
#line 596 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"replacement"};
                    }
#line 2428 "pipeline_parser_gen.cpp"
                    break;

                    case 98:  // aggExprAsUserFieldname: ADD
#line 604 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$add"};
                    }
#line 2436 "pipeline_parser_gen.cpp"
                    break;

                    case 99:  // aggExprAsUserFieldname: ATAN2
#line 607 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$atan2"};
                    }
#line 2444 "pipeline_parser_gen.cpp"
                    break;

                    case 100:  // aggExprAsUserFieldname: AND
#line 610 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$and"};
                    }
#line 2452 "pipeline_parser_gen.cpp"
                    break;

                    case 101:  // aggExprAsUserFieldname: CONST_EXPR
#line 613 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$const"};
                    }
#line 2460 "pipeline_parser_gen.cpp"
                    break;

                    case 102:  // aggExprAsUserFieldname: LITERAL
#line 616 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$literal"};
                    }
#line 2468 "pipeline_parser_gen.cpp"
                    break;

                    case 103:  // aggExprAsUserFieldname: OR
#line 619 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$or"};
                    }
#line 2476 "pipeline_parser_gen.cpp"
                    break;

                    case 104:  // aggExprAsUserFieldname: NOT
#line 622 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$not"};
                    }
#line 2484 "pipeline_parser_gen.cpp"
                    break;

                    case 105:  // aggExprAsUserFieldname: CMP
#line 625 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$cmp"};
                    }
#line 2492 "pipeline_parser_gen.cpp"
                    break;

                    case 106:  // aggExprAsUserFieldname: EQ
#line 628 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$eq"};
                    }
#line 2500 "pipeline_parser_gen.cpp"
                    break;

                    case 107:  // aggExprAsUserFieldname: GT
#line 631 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gt"};
                    }
#line 2508 "pipeline_parser_gen.cpp"
                    break;

                    case 108:  // aggExprAsUserFieldname: GTE
#line 634 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$gte"};
                    }
#line 2516 "pipeline_parser_gen.cpp"
                    break;

                    case 109:  // aggExprAsUserFieldname: LT
#line 637 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lt"};
                    }
#line 2524 "pipeline_parser_gen.cpp"
                    break;

                    case 110:  // aggExprAsUserFieldname: LTE
#line 640 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$lte"};
                    }
#line 2532 "pipeline_parser_gen.cpp"
                    break;

                    case 111:  // aggExprAsUserFieldname: NE
#line 643 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ne"};
                    }
#line 2540 "pipeline_parser_gen.cpp"
                    break;

                    case 112:  // aggExprAsUserFieldname: CONVERT
#line 646 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$convert"};
                    }
#line 2548 "pipeline_parser_gen.cpp"
                    break;

                    case 113:  // aggExprAsUserFieldname: TO_BOOL
#line 649 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toBool"};
                    }
#line 2556 "pipeline_parser_gen.cpp"
                    break;

                    case 114:  // aggExprAsUserFieldname: TO_DATE
#line 652 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDate"};
                    }
#line 2564 "pipeline_parser_gen.cpp"
                    break;

                    case 115:  // aggExprAsUserFieldname: TO_DECIMAL
#line 655 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDecimal"};
                    }
#line 2572 "pipeline_parser_gen.cpp"
                    break;

                    case 116:  // aggExprAsUserFieldname: TO_DOUBLE
#line 658 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toDouble"};
                    }
#line 2580 "pipeline_parser_gen.cpp"
                    break;

                    case 117:  // aggExprAsUserFieldname: TO_INT
#line 661 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toInt"};
                    }
#line 2588 "pipeline_parser_gen.cpp"
                    break;

                    case 118:  // aggExprAsUserFieldname: TO_LONG
#line 664 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLong"};
                    }
#line 2596 "pipeline_parser_gen.cpp"
                    break;

                    case 119:  // aggExprAsUserFieldname: TO_OBJECT_ID
#line 667 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toObjectId"};
                    }
#line 2604 "pipeline_parser_gen.cpp"
                    break;

                    case 120:  // aggExprAsUserFieldname: TO_STRING
#line 670 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toString"};
                    }
#line 2612 "pipeline_parser_gen.cpp"
                    break;

                    case 121:  // aggExprAsUserFieldname: TYPE
#line 673 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$type"};
                    }
#line 2620 "pipeline_parser_gen.cpp"
                    break;

                    case 122:  // aggExprAsUserFieldname: ABS
#line 676 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$abs"};
                    }
#line 2628 "pipeline_parser_gen.cpp"
                    break;

                    case 123:  // aggExprAsUserFieldname: CEIL
#line 679 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ceil"};
                    }
#line 2636 "pipeline_parser_gen.cpp"
                    break;

                    case 124:  // aggExprAsUserFieldname: DIVIDE
#line 682 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$divide"};
                    }
#line 2644 "pipeline_parser_gen.cpp"
                    break;

                    case 125:  // aggExprAsUserFieldname: EXPONENT
#line 685 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$exp"};
                    }
#line 2652 "pipeline_parser_gen.cpp"
                    break;

                    case 126:  // aggExprAsUserFieldname: FLOOR
#line 688 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$floor"};
                    }
#line 2660 "pipeline_parser_gen.cpp"
                    break;

                    case 127:  // aggExprAsUserFieldname: LN
#line 691 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ln"};
                    }
#line 2668 "pipeline_parser_gen.cpp"
                    break;

                    case 128:  // aggExprAsUserFieldname: LOG
#line 694 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log"};
                    }
#line 2676 "pipeline_parser_gen.cpp"
                    break;

                    case 129:  // aggExprAsUserFieldname: LOGTEN
#line 697 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$log10"};
                    }
#line 2684 "pipeline_parser_gen.cpp"
                    break;

                    case 130:  // aggExprAsUserFieldname: MOD
#line 700 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$mod"};
                    }
#line 2692 "pipeline_parser_gen.cpp"
                    break;

                    case 131:  // aggExprAsUserFieldname: MULTIPLY
#line 703 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$multiply"};
                    }
#line 2700 "pipeline_parser_gen.cpp"
                    break;

                    case 132:  // aggExprAsUserFieldname: POW
#line 706 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$pow"};
                    }
#line 2708 "pipeline_parser_gen.cpp"
                    break;

                    case 133:  // aggExprAsUserFieldname: ROUND
#line 709 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$round"};
                    }
#line 2716 "pipeline_parser_gen.cpp"
                    break;

                    case 134:  // aggExprAsUserFieldname: SQRT
#line 712 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$sqrt"};
                    }
#line 2724 "pipeline_parser_gen.cpp"
                    break;

                    case 135:  // aggExprAsUserFieldname: SUBTRACT
#line 715 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$subtract"};
                    }
#line 2732 "pipeline_parser_gen.cpp"
                    break;

                    case 136:  // aggExprAsUserFieldname: TRUNC
#line 718 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trunc"};
                    }
#line 2740 "pipeline_parser_gen.cpp"
                    break;

                    case 137:  // aggExprAsUserFieldname: CONCAT
#line 721 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$concat"};
                    }
#line 2748 "pipeline_parser_gen.cpp"
                    break;

                    case 138:  // aggExprAsUserFieldname: DATE_FROM_STRING
#line 724 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateFromString"};
                    }
#line 2756 "pipeline_parser_gen.cpp"
                    break;

                    case 139:  // aggExprAsUserFieldname: DATE_TO_STRING
#line 727 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$dateToString"};
                    }
#line 2764 "pipeline_parser_gen.cpp"
                    break;

                    case 140:  // aggExprAsUserFieldname: INDEX_OF_BYTES
#line 730 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfBytes"};
                    }
#line 2772 "pipeline_parser_gen.cpp"
                    break;

                    case 141:  // aggExprAsUserFieldname: INDEX_OF_CP
#line 733 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$indexOfCP"};
                    }
#line 2780 "pipeline_parser_gen.cpp"
                    break;

                    case 142:  // aggExprAsUserFieldname: LTRIM
#line 736 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$ltrim"};
                    }
#line 2788 "pipeline_parser_gen.cpp"
                    break;

                    case 143:  // aggExprAsUserFieldname: META
#line 739 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$meta"};
                    }
#line 2796 "pipeline_parser_gen.cpp"
                    break;

                    case 144:  // aggExprAsUserFieldname: REGEX_FIND
#line 742 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFind"};
                    }
#line 2804 "pipeline_parser_gen.cpp"
                    break;

                    case 145:  // aggExprAsUserFieldname: REGEX_FIND_ALL
#line 745 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexFindAll"};
                    }
#line 2812 "pipeline_parser_gen.cpp"
                    break;

                    case 146:  // aggExprAsUserFieldname: REGEX_MATCH
#line 748 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$regexMatch"};
                    }
#line 2820 "pipeline_parser_gen.cpp"
                    break;

                    case 147:  // aggExprAsUserFieldname: REPLACE_ONE
#line 751 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceOne"};
                    }
#line 2828 "pipeline_parser_gen.cpp"
                    break;

                    case 148:  // aggExprAsUserFieldname: REPLACE_ALL
#line 754 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$replaceAll"};
                    }
#line 2836 "pipeline_parser_gen.cpp"
                    break;

                    case 149:  // aggExprAsUserFieldname: RTRIM
#line 757 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$rtrim"};
                    }
#line 2844 "pipeline_parser_gen.cpp"
                    break;

                    case 150:  // aggExprAsUserFieldname: SPLIT
#line 760 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$split"};
                    }
#line 2852 "pipeline_parser_gen.cpp"
                    break;

                    case 151:  // aggExprAsUserFieldname: STR_LEN_BYTES
#line 763 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenBytes"};
                    }
#line 2860 "pipeline_parser_gen.cpp"
                    break;

                    case 152:  // aggExprAsUserFieldname: STR_LEN_CP
#line 766 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strLenCP"};
                    }
#line 2868 "pipeline_parser_gen.cpp"
                    break;

                    case 153:  // aggExprAsUserFieldname: STR_CASE_CMP
#line 769 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$strcasecmp"};
                    }
#line 2876 "pipeline_parser_gen.cpp"
                    break;

                    case 154:  // aggExprAsUserFieldname: SUBSTR
#line 772 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substr"};
                    }
#line 2884 "pipeline_parser_gen.cpp"
                    break;

                    case 155:  // aggExprAsUserFieldname: SUBSTR_BYTES
#line 775 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrBytes"};
                    }
#line 2892 "pipeline_parser_gen.cpp"
                    break;

                    case 156:  // aggExprAsUserFieldname: SUBSTR_CP
#line 778 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$substrCP"};
                    }
#line 2900 "pipeline_parser_gen.cpp"
                    break;

                    case 157:  // aggExprAsUserFieldname: TO_LOWER
#line 781 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toLower"};
                    }
#line 2908 "pipeline_parser_gen.cpp"
                    break;

                    case 158:  // aggExprAsUserFieldname: TRIM
#line 784 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$trim"};
                    }
#line 2916 "pipeline_parser_gen.cpp"
                    break;

                    case 159:  // aggExprAsUserFieldname: TO_UPPER
#line 787 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"$toUpper"};
                    }
#line 2924 "pipeline_parser_gen.cpp"
                    break;

                    case 160:  // string: "string"
#line 794 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserString{YY_MOVE(yystack_[0].value.as<std::string>())}};
                    }
#line 2932 "pipeline_parser_gen.cpp"
                    break;

                    case 161:  // string: "randVal"
#line 799 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"randVal"}};
                    }
#line 2940 "pipeline_parser_gen.cpp"
                    break;

                    case 162:  // string: "textScore"
#line 802 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserString{"textScore"}};
                    }
#line 2948 "pipeline_parser_gen.cpp"
                    break;

                    case 163:  // fieldPath: "$-prefixed string"
#line 808 "pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>());
                        if (str.size() == 1) {
                            error(yystack_[0].location, "'$' by iteslf is not a valid FieldPath");
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str.substr(1), false}};
                    }
#line 2960 "pipeline_parser_gen.cpp"
                    break;

                    case 164:  // variable: "$$-prefixed string"
#line 816 "pipeline_grammar.yy"
                    {
                        std::string str = YY_MOVE(yystack_[0].value.as<std::string>()).substr(2);
                        auto status = c_node_validation::validateVariableName(str);
                        if (!status.isOK()) {
                            error(yystack_[0].location, status.reason());
                        }
                        yylhs.value.as<CNode>() = CNode{UserFieldPath{str, true}};
                    }
#line 2973 "pipeline_parser_gen.cpp"
                    break;

                    case 165:  // binary: "BinData"
#line 825 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserBinary{YY_MOVE(yystack_[0].value.as<BSONBinData>())}};
                    }
#line 2981 "pipeline_parser_gen.cpp"
                    break;

                    case 166:  // undefined: "undefined"
#line 831 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserUndefined{}};
                    }
#line 2989 "pipeline_parser_gen.cpp"
                    break;

                    case 167:  // objectId: "ObjectID"
#line 837 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserObjectId{}};
                    }
#line 2997 "pipeline_parser_gen.cpp"
                    break;

                    case 168:  // date: "Date"
#line 843 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDate{YY_MOVE(yystack_[0].value.as<Date_t>())}};
                    }
#line 3005 "pipeline_parser_gen.cpp"
                    break;

                    case 169:  // null: "null"
#line 849 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserNull{}};
                    }
#line 3013 "pipeline_parser_gen.cpp"
                    break;

                    case 170:  // regex: "regex"
#line 855 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserRegex{YY_MOVE(yystack_[0].value.as<BSONRegEx>())}};
                    }
#line 3021 "pipeline_parser_gen.cpp"
                    break;

                    case 171:  // dbPointer: "dbPointer"
#line 861 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDBPointer{YY_MOVE(yystack_[0].value.as<BSONDBRef>())}};
                    }
#line 3029 "pipeline_parser_gen.cpp"
                    break;

                    case 172:  // javascript: "Code"
#line 867 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserJavascript{YY_MOVE(yystack_[0].value.as<BSONCode>())}};
                    }
#line 3037 "pipeline_parser_gen.cpp"
                    break;

                    case 173:  // symbol: "Symbol"
#line 873 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserSymbol{YY_MOVE(yystack_[0].value.as<BSONSymbol>())}};
                    }
#line 3045 "pipeline_parser_gen.cpp"
                    break;

                    case 174:  // javascriptWScope: "CodeWScope"
#line 879 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserJavascriptWithScope{
                            YY_MOVE(yystack_[0].value.as<BSONCodeWScope>())}};
                    }
#line 3053 "pipeline_parser_gen.cpp"
                    break;

                    case 175:  // timestamp: "Timestamp"
#line 885 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserTimestamp{YY_MOVE(yystack_[0].value.as<Timestamp>())}};
                    }
#line 3061 "pipeline_parser_gen.cpp"
                    break;

                    case 176:  // minKey: "minKey"
#line 891 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMinKey{YY_MOVE(yystack_[0].value.as<UserMinKey>())}};
                    }
#line 3069 "pipeline_parser_gen.cpp"
                    break;

                    case 177:  // maxKey: "maxKey"
#line 897 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserMaxKey{YY_MOVE(yystack_[0].value.as<UserMaxKey>())}};
                    }
#line 3077 "pipeline_parser_gen.cpp"
                    break;

                    case 178:  // int: "arbitrary integer"
#line 903 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserInt{YY_MOVE(yystack_[0].value.as<int>())}};
                    }
#line 3085 "pipeline_parser_gen.cpp"
                    break;

                    case 179:  // int: "zero (int)"
#line 906 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{0}};
                    }
#line 3093 "pipeline_parser_gen.cpp"
                    break;

                    case 180:  // int: "1 (int)"
#line 909 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{1}};
                    }
#line 3101 "pipeline_parser_gen.cpp"
                    break;

                    case 181:  // int: "-1 (int)"
#line 912 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserInt{-1}};
                    }
#line 3109 "pipeline_parser_gen.cpp"
                    break;

                    case 182:  // long: "arbitrary long"
#line 918 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserLong{YY_MOVE(yystack_[0].value.as<long long>())}};
                    }
#line 3117 "pipeline_parser_gen.cpp"
                    break;

                    case 183:  // long: "zero (long)"
#line 921 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{0ll}};
                    }
#line 3125 "pipeline_parser_gen.cpp"
                    break;

                    case 184:  // long: "1 (long)"
#line 924 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{1ll}};
                    }
#line 3133 "pipeline_parser_gen.cpp"
                    break;

                    case 185:  // long: "-1 (long)"
#line 927 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserLong{-1ll}};
                    }
#line 3141 "pipeline_parser_gen.cpp"
                    break;

                    case 186:  // double: "arbitrary double"
#line 933 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDouble{YY_MOVE(yystack_[0].value.as<double>())}};
                    }
#line 3149 "pipeline_parser_gen.cpp"
                    break;

                    case 187:  // double: "zero (double)"
#line 936 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{0.0}};
                    }
#line 3157 "pipeline_parser_gen.cpp"
                    break;

                    case 188:  // double: "1 (double)"
#line 939 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{1.0}};
                    }
#line 3165 "pipeline_parser_gen.cpp"
                    break;

                    case 189:  // double: "-1 (double)"
#line 942 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDouble{-1.0}};
                    }
#line 3173 "pipeline_parser_gen.cpp"
                    break;

                    case 190:  // decimal: "arbitrary decimal"
#line 948 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{UserDecimal{YY_MOVE(yystack_[0].value.as<Decimal128>())}};
                    }
#line 3181 "pipeline_parser_gen.cpp"
                    break;

                    case 191:  // decimal: "zero (decimal)"
#line 951 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{0.0}};
                    }
#line 3189 "pipeline_parser_gen.cpp"
                    break;

                    case 192:  // decimal: "1 (decimal)"
#line 954 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{1.0}};
                    }
#line 3197 "pipeline_parser_gen.cpp"
                    break;

                    case 193:  // decimal: "-1 (decimal)"
#line 957 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserDecimal{-1.0}};
                    }
#line 3205 "pipeline_parser_gen.cpp"
                    break;

                    case 194:  // bool: "true"
#line 963 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{true}};
                    }
#line 3213 "pipeline_parser_gen.cpp"
                    break;

                    case 195:  // bool: "false"
#line 966 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{UserBoolean{false}};
                    }
#line 3221 "pipeline_parser_gen.cpp"
                    break;

                    case 196:  // simpleValue: string
#line 972 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3227 "pipeline_parser_gen.cpp"
                    break;

                    case 197:  // simpleValue: fieldPath
#line 973 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3233 "pipeline_parser_gen.cpp"
                    break;

                    case 198:  // simpleValue: variable
#line 974 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3239 "pipeline_parser_gen.cpp"
                    break;

                    case 199:  // simpleValue: binary
#line 975 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3245 "pipeline_parser_gen.cpp"
                    break;

                    case 200:  // simpleValue: undefined
#line 976 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3251 "pipeline_parser_gen.cpp"
                    break;

                    case 201:  // simpleValue: objectId
#line 977 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3257 "pipeline_parser_gen.cpp"
                    break;

                    case 202:  // simpleValue: date
#line 978 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3263 "pipeline_parser_gen.cpp"
                    break;

                    case 203:  // simpleValue: null
#line 979 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3269 "pipeline_parser_gen.cpp"
                    break;

                    case 204:  // simpleValue: regex
#line 980 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3275 "pipeline_parser_gen.cpp"
                    break;

                    case 205:  // simpleValue: dbPointer
#line 981 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3281 "pipeline_parser_gen.cpp"
                    break;

                    case 206:  // simpleValue: javascript
#line 982 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3287 "pipeline_parser_gen.cpp"
                    break;

                    case 207:  // simpleValue: symbol
#line 983 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3293 "pipeline_parser_gen.cpp"
                    break;

                    case 208:  // simpleValue: javascriptWScope
#line 984 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3299 "pipeline_parser_gen.cpp"
                    break;

                    case 209:  // simpleValue: int
#line 985 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3305 "pipeline_parser_gen.cpp"
                    break;

                    case 210:  // simpleValue: long
#line 986 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3311 "pipeline_parser_gen.cpp"
                    break;

                    case 211:  // simpleValue: double
#line 987 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3317 "pipeline_parser_gen.cpp"
                    break;

                    case 212:  // simpleValue: decimal
#line 988 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3323 "pipeline_parser_gen.cpp"
                    break;

                    case 213:  // simpleValue: bool
#line 989 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3329 "pipeline_parser_gen.cpp"
                    break;

                    case 214:  // simpleValue: timestamp
#line 990 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3335 "pipeline_parser_gen.cpp"
                    break;

                    case 215:  // simpleValue: minKey
#line 991 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3341 "pipeline_parser_gen.cpp"
                    break;

                    case 216:  // simpleValue: maxKey
#line 992 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3347 "pipeline_parser_gen.cpp"
                    break;

                    case 217:  // expressions: %empty
#line 999 "pipeline_grammar.yy"
                    {
                    }
#line 3353 "pipeline_parser_gen.cpp"
                    break;

                    case 218:  // expressions: expression expressions
#line 1000 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 3362 "pipeline_parser_gen.cpp"
                    break;

                    case 219:  // expression: simpleValue
#line 1007 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3368 "pipeline_parser_gen.cpp"
                    break;

                    case 220:  // expression: compoundExpression
#line 1007 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3374 "pipeline_parser_gen.cpp"
                    break;

                    case 221:  // exprFixedTwoArg: "array" expression expression "end of array"
#line 1011 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>()),
                                                       YY_MOVE(yystack_[1].value.as<CNode>())}};
                    }
#line 3382 "pipeline_parser_gen.cpp"
                    break;

                    case 222:  // compoundExpression: expressionArray
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3388 "pipeline_parser_gen.cpp"
                    break;

                    case 223:  // compoundExpression: expressionObject
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3394 "pipeline_parser_gen.cpp"
                    break;

                    case 224:  // compoundExpression: maths
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3400 "pipeline_parser_gen.cpp"
                    break;

                    case 225:  // compoundExpression: boolExps
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3406 "pipeline_parser_gen.cpp"
                    break;

                    case 226:  // compoundExpression: literalEscapes
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3412 "pipeline_parser_gen.cpp"
                    break;

                    case 227:  // compoundExpression: compExprs
#line 1016 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3418 "pipeline_parser_gen.cpp"
                    break;

                    case 228:  // compoundExpression: typeExpression
#line 1017 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3424 "pipeline_parser_gen.cpp"
                    break;

                    case 229:  // compoundExpression: stringExps
#line 1017 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3430 "pipeline_parser_gen.cpp"
                    break;

                    case 230:  // expressionArray: "array" expressions "end of array"
#line 1023 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 3438 "pipeline_parser_gen.cpp"
                    break;

                    case 231:  // expressionObject: "object" expressionFields "end of object"
#line 1031 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 3446 "pipeline_parser_gen.cpp"
                    break;

                    case 232:  // expressionFields: %empty
#line 1037 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 3454 "pipeline_parser_gen.cpp"
                    break;

                    case 233:  // expressionFields: expressionFields expressionField
#line 1040 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 3463 "pipeline_parser_gen.cpp"
                    break;

                    case 234:  // expressionField: expressionFieldname expression
#line 1047 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3471 "pipeline_parser_gen.cpp"
                    break;

                    case 235:  // expressionFieldname: invariableUserFieldname
#line 1054 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3477 "pipeline_parser_gen.cpp"
                    break;

                    case 236:  // expressionFieldname: stageAsUserFieldname
#line 1054 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3483 "pipeline_parser_gen.cpp"
                    break;

                    case 237:  // expressionFieldname: argAsUserFieldname
#line 1054 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3489 "pipeline_parser_gen.cpp"
                    break;

                    case 238:  // expressionFieldname: idAsUserFieldname
#line 1054 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 3495 "pipeline_parser_gen.cpp"
                    break;

                    case 239:  // idAsUserFieldname: ID
#line 1058 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() = UserFieldname{"_id"};
                    }
#line 3503 "pipeline_parser_gen.cpp"
                    break;

                    case 240:  // maths: add
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3509 "pipeline_parser_gen.cpp"
                    break;

                    case 241:  // maths: atan2
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3515 "pipeline_parser_gen.cpp"
                    break;

                    case 242:  // maths: abs
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3521 "pipeline_parser_gen.cpp"
                    break;

                    case 243:  // maths: ceil
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3527 "pipeline_parser_gen.cpp"
                    break;

                    case 244:  // maths: divide
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3533 "pipeline_parser_gen.cpp"
                    break;

                    case 245:  // maths: exponent
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3539 "pipeline_parser_gen.cpp"
                    break;

                    case 246:  // maths: floor
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3545 "pipeline_parser_gen.cpp"
                    break;

                    case 247:  // maths: ln
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3551 "pipeline_parser_gen.cpp"
                    break;

                    case 248:  // maths: log
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3557 "pipeline_parser_gen.cpp"
                    break;

                    case 249:  // maths: logten
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3563 "pipeline_parser_gen.cpp"
                    break;

                    case 250:  // maths: mod
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3569 "pipeline_parser_gen.cpp"
                    break;

                    case 251:  // maths: multiply
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3575 "pipeline_parser_gen.cpp"
                    break;

                    case 252:  // maths: pow
#line 1064 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3581 "pipeline_parser_gen.cpp"
                    break;

                    case 253:  // maths: round
#line 1065 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3587 "pipeline_parser_gen.cpp"
                    break;

                    case 254:  // maths: sqrt
#line 1065 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3593 "pipeline_parser_gen.cpp"
                    break;

                    case 255:  // maths: subtract
#line 1065 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3599 "pipeline_parser_gen.cpp"
                    break;

                    case 256:  // maths: trunc
#line 1065 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3605 "pipeline_parser_gen.cpp"
                    break;

                    case 257:  // add: "object" ADD expressionArray "end of object"
#line 1069 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::add, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3614 "pipeline_parser_gen.cpp"
                    break;

                    case 258:  // atan2: "object" ATAN2 exprFixedTwoArg "end of object"
#line 1076 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::atan2, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3623 "pipeline_parser_gen.cpp"
                    break;

                    case 259:  // abs: "object" ABS expression "end of object"
#line 1082 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::abs, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3631 "pipeline_parser_gen.cpp"
                    break;

                    case 260:  // ceil: "object" CEIL expression "end of object"
#line 1087 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ceil, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3639 "pipeline_parser_gen.cpp"
                    break;

                    case 261:  // divide: "object" DIVIDE "array" expression expression "end of
                               // array" "end of object"
#line 1092 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::divide,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3648 "pipeline_parser_gen.cpp"
                    break;

                    case 262:  // exponent: "object" EXPONENT expression "end of object"
#line 1098 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::exponent, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3656 "pipeline_parser_gen.cpp"
                    break;

                    case 263:  // floor: "object" FLOOR expression "end of object"
#line 1103 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::floor, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3664 "pipeline_parser_gen.cpp"
                    break;

                    case 264:  // ln: "object" LN expression "end of object"
#line 1108 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ln, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3672 "pipeline_parser_gen.cpp"
                    break;

                    case 265:  // log: "object" LOG "array" expression expression "end of array"
                               // "end of object"
#line 1113 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::log,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3681 "pipeline_parser_gen.cpp"
                    break;

                    case 266:  // logten: "object" LOGTEN expression "end of object"
#line 1119 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::logten, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3689 "pipeline_parser_gen.cpp"
                    break;

                    case 267:  // mod: "object" MOD "array" expression expression "end of array"
                               // "end of object"
#line 1124 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::mod,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3698 "pipeline_parser_gen.cpp"
                    break;

                    case 268:  // multiply: "object" MULTIPLY "array" expression expression
                               // expressions "end of array" "end of object"
#line 1130 "pipeline_grammar.yy"
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
#line 3710 "pipeline_parser_gen.cpp"
                    break;

                    case 269:  // pow: "object" POW "array" expression expression "end of array"
                               // "end of object"
#line 1139 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::pow,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3719 "pipeline_parser_gen.cpp"
                    break;

                    case 270:  // round: "object" ROUND "array" expression expression "end of array"
                               // "end of object"
#line 1145 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::round,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3728 "pipeline_parser_gen.cpp"
                    break;

                    case 271:  // sqrt: "object" SQRT expression "end of object"
#line 1151 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::sqrt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3736 "pipeline_parser_gen.cpp"
                    break;

                    case 272:  // subtract: "object" SUBTRACT "array" expression expression "end of
                               // array" "end of object"
#line 1156 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::subtract,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3745 "pipeline_parser_gen.cpp"
                    break;

                    case 273:  // trunc: "object" TRUNC "array" expression expression "end of array"
                               // "end of object"
#line 1162 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trunc,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3754 "pipeline_parser_gen.cpp"
                    break;

                    case 274:  // boolExps: and
#line 1168 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3760 "pipeline_parser_gen.cpp"
                    break;

                    case 275:  // boolExps: or
#line 1168 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3766 "pipeline_parser_gen.cpp"
                    break;

                    case 276:  // boolExps: not
#line 1168 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3772 "pipeline_parser_gen.cpp"
                    break;

                    case 277:  // and: "object" AND expressionArray "end of object"
#line 1172 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::andExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3781 "pipeline_parser_gen.cpp"
                    break;

                    case 278:  // or: "object" OR expressionArray "end of object"
#line 1179 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::orExpr, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 3790 "pipeline_parser_gen.cpp"
                    break;

                    case 279:  // not: "object" NOT "array" expression "end of array" "end of
                               // object"
#line 1186 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::notExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 3799 "pipeline_parser_gen.cpp"
                    break;

                    case 280:  // stringExps: concat
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3805 "pipeline_parser_gen.cpp"
                    break;

                    case 281:  // stringExps: dateFromString
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3811 "pipeline_parser_gen.cpp"
                    break;

                    case 282:  // stringExps: dateToString
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3817 "pipeline_parser_gen.cpp"
                    break;

                    case 283:  // stringExps: indexOfBytes
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3823 "pipeline_parser_gen.cpp"
                    break;

                    case 284:  // stringExps: indexOfCP
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3829 "pipeline_parser_gen.cpp"
                    break;

                    case 285:  // stringExps: ltrim
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3835 "pipeline_parser_gen.cpp"
                    break;

                    case 286:  // stringExps: regexFind
#line 1193 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3841 "pipeline_parser_gen.cpp"
                    break;

                    case 287:  // stringExps: regexFindAll
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3847 "pipeline_parser_gen.cpp"
                    break;

                    case 288:  // stringExps: regexMatch
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3853 "pipeline_parser_gen.cpp"
                    break;

                    case 289:  // stringExps: replaceOne
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3859 "pipeline_parser_gen.cpp"
                    break;

                    case 290:  // stringExps: replaceAll
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3865 "pipeline_parser_gen.cpp"
                    break;

                    case 291:  // stringExps: rtrim
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3871 "pipeline_parser_gen.cpp"
                    break;

                    case 292:  // stringExps: split
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3877 "pipeline_parser_gen.cpp"
                    break;

                    case 293:  // stringExps: strLenBytes
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3883 "pipeline_parser_gen.cpp"
                    break;

                    case 294:  // stringExps: strLenCP
#line 1194 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3889 "pipeline_parser_gen.cpp"
                    break;

                    case 295:  // stringExps: strcasecmp
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3895 "pipeline_parser_gen.cpp"
                    break;

                    case 296:  // stringExps: substr
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3901 "pipeline_parser_gen.cpp"
                    break;

                    case 297:  // stringExps: substrBytes
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3907 "pipeline_parser_gen.cpp"
                    break;

                    case 298:  // stringExps: substrCP
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3913 "pipeline_parser_gen.cpp"
                    break;

                    case 299:  // stringExps: toLower
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3919 "pipeline_parser_gen.cpp"
                    break;

                    case 300:  // stringExps: trim
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3925 "pipeline_parser_gen.cpp"
                    break;

                    case 301:  // stringExps: toUpper
#line 1195 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 3931 "pipeline_parser_gen.cpp"
                    break;

                    case 302:  // concat: "object" CONCAT "array" expressions "end of array" "end of
                               // object"
#line 1199 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::concat, CNode{CNode::ArrayChildren{}}}}};
                        auto&& others = YY_MOVE(yystack_[2].value.as<std::vector<CNode>>());
                        auto&& array =
                            yylhs.value.as<CNode>().objectChildren()[0].second.arrayChildren();
                        array.insert(array.end(), others.begin(), others.end());
                    }
#line 3943 "pipeline_parser_gen.cpp"
                    break;

                    case 303:  // formatArg: %empty
#line 1209 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
                    }
#line 3951 "pipeline_parser_gen.cpp"
                    break;

                    case 304:  // formatArg: "format argument" expression
#line 1212 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::formatArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3959 "pipeline_parser_gen.cpp"
                    break;

                    case 305:  // timezoneArg: %empty
#line 1218 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
                    }
#line 3967 "pipeline_parser_gen.cpp"
                    break;

                    case 306:  // timezoneArg: "timezone argument" expression
#line 1221 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::timezoneArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 3975 "pipeline_parser_gen.cpp"
                    break;

                    case 307:  // dateFromString: "object" DATE_FROM_STRING START_ORDERED_OBJECT
                               // "dateString argument" expression formatArg timezoneArg onErrorArg
                               // onNullArg "end of object" "end of object"
#line 1228 "pipeline_grammar.yy"
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
#line 3985 "pipeline_parser_gen.cpp"
                    break;

                    case 308:  // dateToString: "object" DATE_TO_STRING START_ORDERED_OBJECT "date
                               // argument" expression formatArg timezoneArg onNullArg "end of
                               // object" "end of object"
#line 1237 "pipeline_grammar.yy"
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
#line 3995 "pipeline_parser_gen.cpp"
                    break;

                    case 309:  // exprZeroToTwo: %empty
#line 1245 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() = CNode::ArrayChildren{};
                    }
#line 4003 "pipeline_parser_gen.cpp"
                    break;

                    case 310:  // exprZeroToTwo: expression
#line 1248 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4011 "pipeline_parser_gen.cpp"
                    break;

                    case 311:  // exprZeroToTwo: expression expression
#line 1251 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            CNode::ArrayChildren{YY_MOVE(yystack_[1].value.as<CNode>()),
                                                 YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4019 "pipeline_parser_gen.cpp"
                    break;

                    case 312:  // indexOfBytes: "object" INDEX_OF_BYTES "array" expression
                               // expression exprZeroToTwo "end of array" "end of object"
#line 1258 "pipeline_grammar.yy"
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
#line 4031 "pipeline_parser_gen.cpp"
                    break;

                    case 313:  // indexOfCP: "object" INDEX_OF_CP "array" expression expression
                               // exprZeroToTwo "end of array" "end of object"
#line 1269 "pipeline_grammar.yy"
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
#line 4043 "pipeline_parser_gen.cpp"
                    break;

                    case 314:  // charsArg: %empty
#line 1279 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
                    }
#line 4051 "pipeline_parser_gen.cpp"
                    break;

                    case 315:  // charsArg: "chars argument" expression
#line 1282 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::charsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4059 "pipeline_parser_gen.cpp"
                    break;

                    case 316:  // ltrim: "object" LTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1288 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ltrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4069 "pipeline_parser_gen.cpp"
                    break;

                    case 317:  // rtrim: "object" RTRIM START_ORDERED_OBJECT charsArg "input
                               // argument" expression "end of object" "end of object"
#line 1296 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::rtrim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4079 "pipeline_parser_gen.cpp"
                    break;

                    case 318:  // trim: "object" TRIM START_ORDERED_OBJECT charsArg "input argument"
                               // expression "end of object" "end of object"
#line 1304 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::trim,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[2].value.as<CNode>())},
                                 YY_MOVE(yystack_[4]
                                             .value.as<std::pair<CNode::Fieldname, CNode>>())}}}}};
                    }
#line 4089 "pipeline_parser_gen.cpp"
                    break;

                    case 319:  // optionsArg: %empty
#line 1312 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
                    }
#line 4097 "pipeline_parser_gen.cpp"
                    break;

                    case 320:  // optionsArg: "options argument" expression
#line 1315 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::optionsArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4105 "pipeline_parser_gen.cpp"
                    break;

                    case 321:  // regexArgs: START_ORDERED_OBJECT "input argument" expression
                               // optionsArg "regex argument" expression "end of object"
#line 1320 "pipeline_grammar.yy"
                    {
                        // Note that the order of these arguments must match the constructor for the
                        // regex expression.
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                            {KeyFieldname::regexArg, YY_MOVE(yystack_[1].value.as<CNode>())},
                            YY_MOVE(yystack_[3].value.as<std::pair<CNode::Fieldname, CNode>>())}};
                    }
#line 4117 "pipeline_parser_gen.cpp"
                    break;

                    case 322:  // regexFind: "object" REGEX_FIND regexArgs "end of object"
#line 1329 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFind, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4125 "pipeline_parser_gen.cpp"
                    break;

                    case 323:  // regexFindAll: "object" REGEX_FIND_ALL regexArgs "end of object"
#line 1335 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexFindAll, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4133 "pipeline_parser_gen.cpp"
                    break;

                    case 324:  // regexMatch: "object" REGEX_MATCH regexArgs "end of object"
#line 1341 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::regexMatch, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4141 "pipeline_parser_gen.cpp"
                    break;

                    case 325:  // replaceOne: "object" REPLACE_ONE START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1348 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceOne,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4152 "pipeline_parser_gen.cpp"
                    break;

                    case 326:  // replaceAll: "object" REPLACE_ALL START_ORDERED_OBJECT "find
                               // argument" expression "input argument" expression "replacement
                               // argument" expression "end of object" "end of object"
#line 1358 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::replaceAll,
                             CNode{CNode::ObjectChildren{
                                 {KeyFieldname::inputArg, YY_MOVE(yystack_[4].value.as<CNode>())},
                                 {KeyFieldname::findArg, YY_MOVE(yystack_[6].value.as<CNode>())},
                                 {KeyFieldname::replacementArg,
                                  YY_MOVE(yystack_[2].value.as<CNode>())}}}}}};
                    }
#line 4163 "pipeline_parser_gen.cpp"
                    break;

                    case 327:  // split: "object" SPLIT "array" expression expression "end of array"
                               // "end of object"
#line 1367 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::split,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4172 "pipeline_parser_gen.cpp"
                    break;

                    case 328:  // strLenBytes: "object" STR_LEN_BYTES expression "end of object"
#line 1374 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenBytes, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4181 "pipeline_parser_gen.cpp"
                    break;

                    case 329:  // strLenCP: "object" STR_LEN_CP expression "end of object"
#line 1381 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strLenCP, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4190 "pipeline_parser_gen.cpp"
                    break;

                    case 330:  // strcasecmp: "object" STR_CASE_CMP "array" expression expression
                               // "end of array" "end of object"
#line 1389 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::strcasecmp,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4199 "pipeline_parser_gen.cpp"
                    break;

                    case 331:  // substr: "object" SUBSTR "array" expression expression expression
                               // "end of array" "end of object"
#line 1397 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4208 "pipeline_parser_gen.cpp"
                    break;

                    case 332:  // substrBytes: "object" SUBSTR_BYTES "array" expression expression
                               // expression "end of array" "end of object"
#line 1405 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrBytes,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4217 "pipeline_parser_gen.cpp"
                    break;

                    case 333:  // substrCP: "object" SUBSTR_CP "array" expression expression
                               // expression "end of array" "end of object"
#line 1413 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::substrCP,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[4].value.as<CNode>()),
                                                        YY_MOVE(yystack_[3].value.as<CNode>()),
                                                        YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4226 "pipeline_parser_gen.cpp"
                    break;

                    case 334:  // toLower: "object" TO_LOWER expression "end of object"
#line 1420 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLower, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4234 "pipeline_parser_gen.cpp"
                    break;

                    case 335:  // toUpper: "object" TO_UPPER expression "end of object"
#line 1426 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toUpper, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4242 "pipeline_parser_gen.cpp"
                    break;

                    case 336:  // metaSortKeyword: "randVal"
#line 1432 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::randVal};
                    }
#line 4250 "pipeline_parser_gen.cpp"
                    break;

                    case 337:  // metaSortKeyword: "textScore"
#line 1435 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::textScore};
                    }
#line 4258 "pipeline_parser_gen.cpp"
                    break;

                    case 338:  // metaSort: "object" META metaSortKeyword "end of object"
#line 1441 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::meta, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4266 "pipeline_parser_gen.cpp"
                    break;

                    case 339:  // sortSpecs: "object" specList "end of object"
#line 1447 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4274 "pipeline_parser_gen.cpp"
                    break;

                    case 340:  // specList: %empty
#line 1452 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4282 "pipeline_parser_gen.cpp"
                    break;

                    case 341:  // specList: specList sortSpec
#line 1455 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4291 "pipeline_parser_gen.cpp"
                    break;

                    case 342:  // oneOrNegOne: "1 (int)"
#line 1462 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intOneKey};
                    }
#line 4299 "pipeline_parser_gen.cpp"
                    break;

                    case 343:  // oneOrNegOne: "-1 (int)"
#line 1465 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::intNegOneKey};
                    }
#line 4307 "pipeline_parser_gen.cpp"
                    break;

                    case 344:  // oneOrNegOne: "1 (long)"
#line 1468 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longOneKey};
                    }
#line 4315 "pipeline_parser_gen.cpp"
                    break;

                    case 345:  // oneOrNegOne: "-1 (long)"
#line 1471 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::longNegOneKey};
                    }
#line 4323 "pipeline_parser_gen.cpp"
                    break;

                    case 346:  // oneOrNegOne: "1 (double)"
#line 1474 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleOneKey};
                    }
#line 4331 "pipeline_parser_gen.cpp"
                    break;

                    case 347:  // oneOrNegOne: "-1 (double)"
#line 1477 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::doubleNegOneKey};
                    }
#line 4339 "pipeline_parser_gen.cpp"
                    break;

                    case 348:  // oneOrNegOne: "1 (decimal)"
#line 1480 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalOneKey};
                    }
#line 4347 "pipeline_parser_gen.cpp"
                    break;

                    case 349:  // oneOrNegOne: "-1 (decimal)"
#line 1483 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{KeyValue::decimalNegOneKey};
                    }
#line 4355 "pipeline_parser_gen.cpp"
                    break;

                    case 350:  // sortSpec: valueFieldname metaSort
#line 1488 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4363 "pipeline_parser_gen.cpp"
                    break;

                    case 351:  // sortSpec: valueFieldname oneOrNegOne
#line 1490 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4371 "pipeline_parser_gen.cpp"
                    break;

                    case 352:  // literalEscapes: const
#line 1496 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4377 "pipeline_parser_gen.cpp"
                    break;

                    case 353:  // literalEscapes: literal
#line 1496 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4383 "pipeline_parser_gen.cpp"
                    break;

                    case 354:  // const: "object" CONST_EXPR "array" value "end of array" "end of
                               // object"
#line 1500 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::constExpr,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4392 "pipeline_parser_gen.cpp"
                    break;

                    case 355:  // literal: "object" LITERAL "array" value "end of array" "end of
                               // object"
#line 1507 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::literal,
                             CNode{CNode::ArrayChildren{YY_MOVE(yystack_[2].value.as<CNode>())}}}}};
                    }
#line 4401 "pipeline_parser_gen.cpp"
                    break;

                    case 356:  // value: simpleValue
#line 1514 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4407 "pipeline_parser_gen.cpp"
                    break;

                    case 357:  // value: compoundValue
#line 1514 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4413 "pipeline_parser_gen.cpp"
                    break;

                    case 358:  // compoundValue: valueArray
#line 1518 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4419 "pipeline_parser_gen.cpp"
                    break;

                    case 359:  // compoundValue: valueObject
#line 1518 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4425 "pipeline_parser_gen.cpp"
                    break;

                    case 360:  // valueArray: "array" values "end of array"
#line 1522 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() =
                            CNode{YY_MOVE(yystack_[1].value.as<std::vector<CNode>>())};
                    }
#line 4433 "pipeline_parser_gen.cpp"
                    break;

                    case 361:  // values: %empty
#line 1528 "pipeline_grammar.yy"
                    {
                    }
#line 4439 "pipeline_parser_gen.cpp"
                    break;

                    case 362:  // values: value values
#line 1529 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::vector<CNode>>() =
                            YY_MOVE(yystack_[0].value.as<std::vector<CNode>>());
                        yylhs.value.as<std::vector<CNode>>().emplace_back(
                            YY_MOVE(yystack_[1].value.as<CNode>()));
                    }
#line 4448 "pipeline_parser_gen.cpp"
                    break;

                    case 363:  // valueObject: "object" valueFields "end of object"
#line 1536 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                    }
#line 4456 "pipeline_parser_gen.cpp"
                    break;

                    case 364:  // valueFields: %empty
#line 1542 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode::noopLeaf();
                    }
#line 4464 "pipeline_parser_gen.cpp"
                    break;

                    case 365:  // valueFields: valueFields valueField
#line 1545 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[1].value.as<CNode>());
                        yylhs.value.as<CNode>().objectChildren().emplace_back(
                            YY_MOVE(yystack_[0].value.as<std::pair<CNode::Fieldname, CNode>>()));
                    }
#line 4473 "pipeline_parser_gen.cpp"
                    break;

                    case 366:  // valueField: valueFieldname value
#line 1552 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = {
                            YY_MOVE(yystack_[1].value.as<CNode::Fieldname>()),
                            YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4481 "pipeline_parser_gen.cpp"
                    break;

                    case 367:  // valueFieldname: invariableUserFieldname
#line 1559 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4487 "pipeline_parser_gen.cpp"
                    break;

                    case 368:  // valueFieldname: stageAsUserFieldname
#line 1560 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4493 "pipeline_parser_gen.cpp"
                    break;

                    case 369:  // valueFieldname: argAsUserFieldname
#line 1561 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4499 "pipeline_parser_gen.cpp"
                    break;

                    case 370:  // valueFieldname: aggExprAsUserFieldname
#line 1562 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4505 "pipeline_parser_gen.cpp"
                    break;

                    case 371:  // valueFieldname: idAsUserFieldname
#line 1563 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode::Fieldname>() =
                            YY_MOVE(yystack_[0].value.as<CNode::Fieldname>());
                    }
#line 4511 "pipeline_parser_gen.cpp"
                    break;

                    case 372:  // compExprs: cmp
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4517 "pipeline_parser_gen.cpp"
                    break;

                    case 373:  // compExprs: eq
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4523 "pipeline_parser_gen.cpp"
                    break;

                    case 374:  // compExprs: gt
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4529 "pipeline_parser_gen.cpp"
                    break;

                    case 375:  // compExprs: gte
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4535 "pipeline_parser_gen.cpp"
                    break;

                    case 376:  // compExprs: lt
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4541 "pipeline_parser_gen.cpp"
                    break;

                    case 377:  // compExprs: lte
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4547 "pipeline_parser_gen.cpp"
                    break;

                    case 378:  // compExprs: ne
#line 1566 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4553 "pipeline_parser_gen.cpp"
                    break;

                    case 379:  // cmp: "object" CMP exprFixedTwoArg "end of object"
#line 1568 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::cmp, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4562 "pipeline_parser_gen.cpp"
                    break;

                    case 380:  // eq: "object" EQ exprFixedTwoArg "end of object"
#line 1573 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::eq, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4571 "pipeline_parser_gen.cpp"
                    break;

                    case 381:  // gt: "object" GT exprFixedTwoArg "end of object"
#line 1578 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4580 "pipeline_parser_gen.cpp"
                    break;

                    case 382:  // gte: "object" GTE exprFixedTwoArg "end of object"
#line 1583 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::gte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4589 "pipeline_parser_gen.cpp"
                    break;

                    case 383:  // lt: "object" LT exprFixedTwoArg "end of object"
#line 1588 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4598 "pipeline_parser_gen.cpp"
                    break;

                    case 384:  // lte: "object" LTE exprFixedTwoArg "end of object"
#line 1593 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::lte, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4607 "pipeline_parser_gen.cpp"
                    break;

                    case 385:  // ne: "object" NE exprFixedTwoArg "end of object"
#line 1598 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::ne, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4616 "pipeline_parser_gen.cpp"
                    break;

                    case 386:  // typeExpression: convert
#line 1604 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4622 "pipeline_parser_gen.cpp"
                    break;

                    case 387:  // typeExpression: toBool
#line 1605 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4628 "pipeline_parser_gen.cpp"
                    break;

                    case 388:  // typeExpression: toDate
#line 1606 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4634 "pipeline_parser_gen.cpp"
                    break;

                    case 389:  // typeExpression: toDecimal
#line 1607 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4640 "pipeline_parser_gen.cpp"
                    break;

                    case 390:  // typeExpression: toDouble
#line 1608 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4646 "pipeline_parser_gen.cpp"
                    break;

                    case 391:  // typeExpression: toInt
#line 1609 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4652 "pipeline_parser_gen.cpp"
                    break;

                    case 392:  // typeExpression: toLong
#line 1610 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4658 "pipeline_parser_gen.cpp"
                    break;

                    case 393:  // typeExpression: toObjectId
#line 1611 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4664 "pipeline_parser_gen.cpp"
                    break;

                    case 394:  // typeExpression: toString
#line 1612 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4670 "pipeline_parser_gen.cpp"
                    break;

                    case 395:  // typeExpression: type
#line 1613 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = YY_MOVE(yystack_[0].value.as<CNode>());
                    }
#line 4676 "pipeline_parser_gen.cpp"
                    break;

                    case 396:  // onErrorArg: %empty
#line 1618 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
                    }
#line 4684 "pipeline_parser_gen.cpp"
                    break;

                    case 397:  // onErrorArg: "onError argument" expression
#line 1621 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onErrorArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4692 "pipeline_parser_gen.cpp"
                    break;

                    case 398:  // onNullArg: %empty
#line 1628 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() =
                            std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
                    }
#line 4700 "pipeline_parser_gen.cpp"
                    break;

                    case 399:  // onNullArg: "onNull argument" expression
#line 1631 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<std::pair<CNode::Fieldname, CNode>>() = std::pair{
                            KeyFieldname::onNullArg, YY_MOVE(yystack_[0].value.as<CNode>())};
                    }
#line 4708 "pipeline_parser_gen.cpp"
                    break;

                    case 400:  // convert: "object" CONVERT START_ORDERED_OBJECT "input argument"
                               // expression onErrorArg onNullArg "to argument" expression "end of
                               // object" "end of object"
#line 1638 "pipeline_grammar.yy"
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
#line 4719 "pipeline_parser_gen.cpp"
                    break;

                    case 401:  // toBool: "object" TO_BOOL expression "end of object"
#line 1647 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toBool, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4727 "pipeline_parser_gen.cpp"
                    break;

                    case 402:  // toDate: "object" TO_DATE expression "end of object"
#line 1652 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDate, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4735 "pipeline_parser_gen.cpp"
                    break;

                    case 403:  // toDecimal: "object" TO_DECIMAL expression "end of object"
#line 1657 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDecimal, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4743 "pipeline_parser_gen.cpp"
                    break;

                    case 404:  // toDouble: "object" TO_DOUBLE expression "end of object"
#line 1662 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toDouble, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4751 "pipeline_parser_gen.cpp"
                    break;

                    case 405:  // toInt: "object" TO_INT expression "end of object"
#line 1667 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toInt, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4759 "pipeline_parser_gen.cpp"
                    break;

                    case 406:  // toLong: "object" TO_LONG expression "end of object"
#line 1672 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toLong, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4767 "pipeline_parser_gen.cpp"
                    break;

                    case 407:  // toObjectId: "object" TO_OBJECT_ID expression "end of object"
#line 1677 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toObjectId, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4775 "pipeline_parser_gen.cpp"
                    break;

                    case 408:  // toString: "object" TO_STRING expression "end of object"
#line 1682 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::toString, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4783 "pipeline_parser_gen.cpp"
                    break;

                    case 409:  // type: "object" TYPE expression "end of object"
#line 1687 "pipeline_grammar.yy"
                    {
                        yylhs.value.as<CNode>() = CNode{CNode::ObjectChildren{
                            {KeyFieldname::type, YY_MOVE(yystack_[1].value.as<CNode>())}}};
                    }
#line 4791 "pipeline_parser_gen.cpp"
                    break;


#line 4795 "pipeline_parser_gen.cpp"

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

void PipelineParserGen::error(const syntax_error& yyexc) {
    error(yyexc.location, yyexc.what());
}

/* Return YYSTR after stripping away unnecessary quotes and
   backslashes, so that it's suitable for yyerror.  The heuristic is
   that double-quoting is unnecessary unless the string contains an
   apostrophe, a comma, or backslash (other than backslash-backslash).
   YYSTR is taken from yytname.  */
std::string PipelineParserGen::yytnamerr_(const char* yystr) {
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

std::string PipelineParserGen::symbol_name(symbol_kind_type yysymbol) {
    return yytnamerr_(yytname_[yysymbol]);
}


// PipelineParserGen::context.
PipelineParserGen::context::context(const PipelineParserGen& yyparser, const symbol_type& yyla)
    : yyparser_(yyparser), yyla_(yyla) {}

int PipelineParserGen::context::expected_tokens(symbol_kind_type yyarg[], int yyargn) const {
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


int PipelineParserGen::yy_syntax_error_arguments_(const context& yyctx,
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
std::string PipelineParserGen::yysyntax_error_(const context& yyctx) const {
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


const short PipelineParserGen::yypact_ninf_ = -550;

const signed char PipelineParserGen::yytable_ninf_ = -1;

const short PipelineParserGen::yypact_[] = {
    -108, -65,  -69,  -61,  57,   -27,  -550, -550, -550, -550, -550, -550, 56,   53,   135,  23,
    -24,  136,  81,   85,   136,  -550, 142,  -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, 760,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    127,  -550, 169,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, 196,  -550, 237,  166,  -27,  -550, -550,
    -550, 760,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, 207,  -550, -550, -550, 220,  136,  -48,  -550, -550,
    760,  238,  674,  40,   -550, 1077, 1077, -550, -550, -550, -550, -550, 236,  262,  -550, -550,
    -550, 760,  -550, -550, -550, 249,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, 866,  992,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, 36,   -550, -550, -550, 866,  -550, 266,  866,  221,  221,  229,  866,  229,  244,  246,
    -550, -550, -550, 247,  229,  866,  866,  229,  229,  248,  250,  251,  866,  253,  866,  229,
    229,  -550, 254,  257,  229,  259,  221,  260,  -550, -550, -550, -550, -550, 261,  -550, 269,
    866,  270,  866,  866,  271,  272,  273,  274,  866,  866,  866,  866,  866,  866,  866,  866,
    866,  866,  -550, 275,  866,  1198, 294,  -550, -550, 307,  321,  322,  866,  323,  324,  325,
    866,  760,  354,  359,  361,  866,  330,  332,  333,  334,  336,  866,  866,  760,  339,  866,
    341,  342,  343,  380,  866,  866,  347,  866,  348,  866,  352,  382,  355,  356,  387,  388,
    866,  380,  866,  363,  866,  364,  365,  866,  866,  866,  866,  366,  370,  372,  375,  376,
    377,  378,  389,  390,  392,  380,  866,  393,  -550, 866,  -550, -550, -550, -550, -550, -550,
    -550, -550, -550, 866,  -550, -550, -550, 360,  395,  866,  866,  866,  866,  -550, -550, -550,
    -550, -550, 866,  866,  396,  -550, 866,  -550, -550, -550, 866,  424,  866,  866,  -550, 398,
    -550, 866,  -550, 866,  -550, -550, 866,  866,  866,  426,  866,  -550, 866,  -550, -550, 866,
    866,  866,  866,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, 428,  866,  -550,
    -550, 402,  403,  404,  429,  434,  434,  407,  866,  866,  409,  408,  -550, 866,  411,  866,
    412,  414,  436,  444,  445,  420,  866,  421,  422,  866,  866,  866,  423,  866,  430,  -550,
    -550, -550, 866,  451,  866,  447,  447,  431,  866,  433,  435,  -550, 438,  440,  441,  437,
    -550, 446,  866,  453,  866,  866,  448,  449,  450,  452,  454,  455,  456,  458,  459,  461,
    -550, 866,  466,  -550, 866,  429,  451,  -550, -550, 462,  463,  -550, 464,  -550, 465,  -550,
    -550, 866,  473,  478,  -550, 467,  -550, -550, 468,  469,  471,  -550, 472,  -550, -550, 866,
    -550, 451,  474,  -550, -550, -550, -550, 475,  866,  866,  -550, -550, -550, -550, -550, 480,
    481,  482,  -550, 483,  484,  487,  488,  -550, 490,  491,  -550, -550, -550, -550};

const short PipelineParserGen::yydefact_[] = {
    0,   0,   0,   0,   0,   6,   2,   68,  3,   340, 4,   1,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   8,   0,   10,  11,  12,  13,  14,  15,  5,   93,  82,  92,  89,  96,  90,  85,  87,
    88,  95,  83,  94,  97,  84,  91,  86,  67,  239, 75,  0,   74,  73,  72,  69,  122, 98,  100,
    99,  123, 105, 137, 101, 112, 138, 139, 124, 339, 106, 125, 126, 107, 108, 140, 141, 102, 127,
    128, 129, 109, 110, 142, 143, 130, 131, 111, 104, 103, 132, 144, 145, 146, 148, 147, 133, 149,
    150, 134, 76,  79,  80,  81,  78,  77,  153, 151, 152, 154, 155, 156, 135, 113, 114, 115, 116,
    117, 118, 157, 119, 120, 159, 158, 136, 121, 368, 369, 370, 367, 371, 0,   341, 0,   193, 192,
    191, 189, 188, 187, 181, 180, 179, 185, 184, 183, 178, 182, 186, 190, 19,  20,  21,  22,  24,
    26,  0,   23,  0,   0,   6,   195, 194, 161, 361, 364, 162, 160, 165, 166, 167, 168, 169, 170,
    171, 172, 173, 174, 175, 176, 177, 163, 164, 205, 206, 207, 208, 209, 214, 210, 211, 212, 215,
    216, 71,  196, 197, 199, 200, 201, 213, 202, 203, 204, 356, 357, 358, 359, 198, 70,  349, 348,
    347, 346, 343, 342, 345, 344, 0,   350, 351, 17,  0,   0,   0,   9,   7,   361, 0,   0,   0,
    25,  0,   0,   64,  65,  66,  63,  27,  0,   0,   362, 360, 363, 0,   365, 336, 337, 0,   58,
    57,  54,  53,  56,  50,  49,  52,  42,  41,  44,  46,  45,  48,  217, 232, 43,  47,  51,  55,
    37,  38,  39,  40,  59,  60,  61,  30,  31,  32,  33,  34,  35,  36,  28,  62,  222, 223, 224,
    240, 241, 225, 274, 275, 276, 226, 352, 353, 229, 280, 281, 282, 283, 284, 285, 286, 287, 288,
    289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 301, 300, 227, 372, 373, 374, 375, 376,
    377, 378, 228, 386, 387, 388, 389, 390, 391, 392, 393, 394, 395, 242, 243, 244, 245, 246, 247,
    248, 249, 250, 251, 252, 253, 254, 255, 256, 29,  16,  0,   366, 338, 219, 217, 220, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   8,   8,   8,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   8,   0,   0,   0,   0,   0,   0,   8,   8,   8,   8,   8,   0,   8,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    8,   0,   0,   0,   0,   218, 230, 0,   0,   0,   0,   0,   0,   0,   217, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   314, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   314, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   314, 0,   0,   231, 0,   236, 237, 235, 238,
    233, 18,  259, 257, 277, 0,   258, 260, 379, 0,   0,   0,   0,   0,   0,   380, 262, 263, 381,
    382, 0,   0,   0,   264, 0,   266, 383, 384, 0,   0,   0,   0,   385, 0,   278, 0,   322, 0,
    323, 324, 0,   0,   0,   0,   0,   271, 0,   328, 329, 0,   0,   0,   0,   401, 402, 403, 404,
    405, 406, 334, 407, 408, 335, 0,   0,   409, 234, 0,   0,   0,   396, 303, 303, 0,   309, 309,
    0,   0,   315, 0,   0,   217, 0,   0,   319, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   221, 302, 354, 0,   398, 0,   305, 305, 0,   310, 0,   0,   355, 0,   0,   0,   0,
    279, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   397, 0,   0,
    304, 0,   396, 398, 261, 311, 0,   0,   265, 0,   267, 0,   269, 320, 0,   0,   0,   270, 0,
    327, 330, 0,   0,   0,   272, 0,   273, 399, 0,   306, 398, 0,   312, 313, 316, 268, 0,   0,
    0,   317, 331, 332, 333, 318, 0,   0,   0,   321, 0,   0,   0,   0,   308, 0,   0,   400, 307,
    326, 325};

const short PipelineParserGen::yypgoto_[] = {
    -550, -550, -550, -204, -550, -14,  287,  -13,  -12,  306,  -550, -550, -550, -550, -174,
    -97,  -68,  -51,  -9,   -41,  -8,   -10,  -4,   -39,  -34,  -43,  -64,  -550, -30,  -28,
    -26,  -550, -22,  -11,  24,   -44,  -550, -550, -550, -550, -550, -550, 316,  -550, -550,
    -550, -550, -550, -550, -550, -550, 283,  -6,   11,   41,   -35,  -343, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -173, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -550,
    -550, -550, -550, -550, -550, -550, -550, -95,  -549, -29,  -60,  -405, -550, -353, 315,
    -25,  -550, -550, -550, -550, -550, -550, -550, -550, -550, -550, -18,  -550};

const short PipelineParserGen::yydefgoto_[] = {
    -1,  230, 489, 123, 49,  124, 125, 126, 127, 128, 235, 494, 242, 53,  180, 181, 182, 183, 184,
    185, 186, 187, 188, 189, 190, 224, 192, 193, 194, 195, 196, 197, 198, 199, 200, 356, 202, 203,
    204, 226, 205, 6,   13,  22,  23,  24,  25,  26,  27,  28,  219, 280, 151, 357, 358, 429, 282,
    283, 421, 284, 285, 286, 287, 288, 289, 290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 300,
    301, 302, 303, 458, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318,
    319, 320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337,
    338, 339, 340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 595, 626, 597, 629, 523, 611,
    359, 225, 601, 8,   14,  206, 10,  15,  216, 217, 245, 129, 4,   459, 156};

const short PipelineParserGen::yytable_[] = {
    50,  51,  52,  155, 423, 201, 191, 149, 147, 148, 149, 147, 148, 150, 154, 231, 150, 7,   426,
    427, 5,   160, 1,   2,   3,   9,   54,  55,  56,  30,  31,  32,  33,  34,  35,  36,  37,  38,
    39,  40,  41,  42,  43,  44,  45,  57,  163, 456, 58,  59,  60,  61,  62,  63,  64,  266, 266,
    11,  65,  12,  537, 164, 130, 66,  67,  68,  69,  70,  71,  47,  72,  73,  134, 135, 136, 74,
    75,  76,  77,  503, 557, 658, 78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  29,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 672, 243, 103, 104, 105, 106,
    107, 108, 109, 201, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 48,  267,
    267, 244, 16,  17,  18,  19,  20,  21,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  237, 145, 207, 208, 268, 268, 209, 210, 273, 273, 152, 131, 132, 133,
    153, 134, 135, 136, 46,  211, 212, 269, 269, 201, 47,  157, 213, 214, 137, 138, 139, 270, 270,
    271, 271, 140, 141, 142, 272, 272, 201, 354, 274, 274, 275, 275, 276, 276, 232, 234, 277, 277,
    218, 149, 147, 148, 215, 236, 220, 150, 490, 278, 278, 460, 461, 607, 54,  55,  56,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  57,  48,  221, 58,  59,
    60,  61,  62,  63,  64,  222, 279, 279, 65,  143, 144, 145, 146, 228, 67,  68,  69,  70,  71,
    229, 72,  73,  227, 281, 281, 74,  75,  76,  77,  352, 239, 353, 78,  79,  80,  81,  82,  83,
    84,  85,  86,  87,  355, 88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101,
    102, 424, 260, 103, 104, 105, 106, 107, 108, 109, 428, 110, 111, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 48,  432, 431, 433, 437, 443, 495, 444, 445, 438, 447, 452, 441, 442,
    453, 422, 455, 457, 464, 496, 449, 450, 434, 435, 436, 454, 466, 468, 471, 472, 473, 474, 486,
    497, 498, 500, 501, 502, 505, 451, 506, 507, 509, 425, 510, 511, 512, 430, 513, 462, 463, 517,
    465, 519, 520, 521, 439, 440, 522, 526, 528, 201, 504, 446, 530, 448, 531, 532, 533, 534, 535,
    562, 485, 201, 516, 539, 541, 542, 547, 491, 492, 493, 548, 467, 549, 469, 470, 550, 551, 552,
    553, 475, 476, 477, 478, 479, 480, 481, 482, 483, 484, 554, 555, 487, 556, 559, 563, 570, 573,
    576, 582, 499, 589, 591, 594, 592, 593, 596, 599, 604, 508, 603, 606, 610, 608, 609, 514, 515,
    612, 613, 518, 614, 616, 617, 621, 524, 525, 625, 527, 628, 529, 623, 641, 631, 633, 223, 634,
    536, 638, 538, 635, 540, 636, 637, 543, 544, 545, 546, 639, 655, 644, 645, 646, 664, 647, 648,
    649, 650, 665, 558, 651, 652, 560, 653, 659, 660, 661, 662, 233, 666, 667, 668, 561, 669, 670,
    351, 673, 674, 564, 565, 566, 567, 677, 678, 679, 680, 681, 568, 569, 682, 683, 571, 684, 685,
    241, 572, 657, 574, 575, 598, 630, 238, 577, 0,   578, 0,   602, 579, 580, 581, 0,   583, 0,
    584, 0,   0,   585, 586, 587, 588, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   590,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   600, 600, 0,   0,   0,   605, 0,   0,   0,   0,
    0,   0,   0,   0,   615, 0,   0,   618, 619, 620, 0,   622, 0,   0,   0,   0,   624, 0,   627,
    0,   0,   0,   632, 0,   0,   0,   0,   0,   0,   0,   0,   0,   640, 0,   642, 643, 0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   654, 0,   0,   656, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   663, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   671, 0,   0,   0,   0,   0,   0,   0,   0,   675, 676, 54,  55,  56,  30,  31,  32,  33,
    34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  57,  0,   0,   58,  59,  60,  61,
    62,  63,  64,  0,   0,   0,   65,  0,   0,   0,   0,   240, 67,  68,  69,  70,  71,  47,  72,
    73,  0,   0,   0,   74,  75,  76,  77,  0,   0,   0,   78,  79,  80,  81,  82,  83,  84,  85,
    86,  87,  0,   88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 0,
    0,   103, 104, 105, 106, 107, 108, 109, 0,   110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 48,  158, 159, 0,   0,   0,   0,   0,   0,   0,   131, 132, 133, 0,   134, 135,
    136, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   137, 138, 139, 0,   0,   0,   0,   140,
    141, 142, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   160, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   161, 162, 0,   0,   0,   0,   0,   0,   0,   163,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   164, 165, 166, 167, 168,
    169, 170, 171, 172, 173, 174, 143, 144, 145, 146, 175, 176, 177, 178, 179, 158, 159, 0,   0,
    0,   0,   0,   0,   0,   131, 132, 133, 0,   134, 135, 136, 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   137, 138, 139, 0,   0,   0,   0,   140, 141, 142, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   160, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   260, 261, 0,   0,   0,   0,   0,   0,   0,   163, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 143, 144,
    145, 146, 175, 176, 177, 178, 179, 360, 361, 362, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   363, 0,   0,   364, 365, 366, 367, 368, 369, 370, 0,   0,
    0,   371, 0,   0,   0,   0,   0,   372, 373, 374, 375, 376, 0,   377, 378, 0,   0,   0,   379,
    380, 381, 382, 0,   0,   0,   383, 384, 385, 0,   386, 387, 388, 389, 390, 391, 0,   392, 393,
    394, 395, 396, 397, 398, 399, 400, 0,   0,   0,   0,   0,   0,   0,   0,   401, 402, 403, 404,
    405, 406, 407, 0,   408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 418, 419, 420, 246, 247,
    0,   0,   0,   0,   0,   0,   0,   248, 249, 250, 0,   251, 252, 253, 0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   254, 255, 256, 0,   0,   0,   0,   257, 258, 259, 0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   160, 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   260, 261, 0,   0,   0,   0,   0,   0,   0,   163, 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174,
    262, 263, 264, 265, 175, 176, 177, 30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   488, 0,   0,   0,   0,   0,   47,  0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   97,  98,  99,  100, 101, 102, 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   48};

const short PipelineParserGen::yycheck_[] = {
    14,  14,  14,  21,  357, 49,  49,  17,  17,  17,  20,  20,  20,  17,  20,  219, 20,  86,  361,
    362, 85,  69,  130, 131, 132, 86,  3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  94,  390, 25,  26,  27,  28,  29,  30,  31,  229, 230,
    0,   35,  86,  465, 109, 86,  40,  41,  42,  43,  44,  45,  46,  47,  48,  36,  37,  38,  52,
    53,  54,  55,  432, 485, 630, 59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  39,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  657, 69,  87,  88,  89,  90,
    91,  92,  93,  161, 95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 229,
    230, 94,  79,  80,  81,  82,  83,  84,  6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,
    17,  18,  19,  20,  21,  221, 122, 32,  33,  229, 230, 36,  37,  229, 230, 86,  32,  33,  34,
    86,  36,  37,  38,  40,  49,  50,  229, 230, 224, 46,  40,  56,  57,  49,  50,  51,  229, 230,
    229, 230, 56,  57,  58,  229, 230, 241, 241, 229, 230, 229, 230, 229, 230, 219, 219, 229, 230,
    40,  220, 220, 220, 86,  220, 19,  220, 421, 229, 230, 393, 394, 575, 3,   4,   5,   6,   7,
    8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  108, 7,   25,  26,
    27,  28,  29,  30,  31,  86,  229, 230, 35,  120, 121, 122, 123, 40,  41,  42,  43,  44,  45,
    46,  47,  48,  62,  229, 230, 52,  53,  54,  55,  40,  39,  16,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  40,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,
    84,  39,  85,  87,  88,  89,  90,  91,  92,  93,  85,  95,  96,  97,  98,  99,  100, 101, 102,
    103, 104, 105, 106, 107, 108, 85,  365, 85,  85,  85,  40,  85,  85,  372, 85,  85,  375, 376,
    85,  353, 85,  85,  85,  40,  383, 384, 368, 369, 370, 388, 85,  85,  85,  85,  85,  85,  85,
    40,  40,  40,  40,  40,  12,  385, 9,   8,   40,  360, 40,  40,  40,  364, 40,  395, 396, 40,
    398, 40,  40,  40,  373, 374, 6,   40,  40,  433, 433, 380, 40,  382, 12,  40,  40,  10,  10,
    39,  418, 445, 445, 40,  40,  40,  40,  421, 421, 421, 40,  400, 40,  402, 403, 40,  40,  40,
    40,  408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 40,  40,  420, 40,  40,  39,  39,  12,
    39,  12,  428, 12,  39,  13,  40,  40,  11,  39,  39,  437, 40,  39,  15,  40,  39,  443, 444,
    12,  12,  447, 39,  39,  39,  39,  452, 453, 14,  455, 20,  457, 39,  17,  40,  39,  157, 39,
    464, 39,  466, 40,  468, 40,  40,  471, 472, 473, 474, 40,  21,  40,  40,  40,  18,  40,  39,
    39,  39,  18,  486, 40,  40,  489, 40,  40,  40,  40,  40,  219, 40,  40,  40,  499, 40,  40,
    230, 40,  40,  505, 506, 507, 508, 40,  40,  40,  40,  40,  514, 515, 40,  40,  518, 40,  40,
    226, 522, 629, 524, 525, 566, 598, 224, 529, -1,  531, -1,  569, 534, 535, 536, -1,  538, -1,
    540, -1,  -1,  543, 544, 545, 546, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  558,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  568, 569, -1,  -1,  -1,  573, -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  582, -1,  -1,  585, 586, 587, -1,  589, -1,  -1,  -1,  -1,  594, -1,  596,
    -1,  -1,  -1,  600, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  610, -1,  612, 613, -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  625, -1,  -1,  628, -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  641, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  655, -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  664, 665, 3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  -1,  -1,  25,  26,  27,  28,
    29,  30,  31,  -1,  -1,  -1,  35,  -1,  -1,  -1,  -1,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  -1,  -1,  -1,  52,  53,  54,  55,  -1,  -1,  -1,  59,  60,  61,  62,  63,  64,  65,  66,
    67,  68,  -1,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  -1,
    -1,  87,  88,  89,  90,  91,  92,  93,  -1,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 23,  24,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  33,  34,  -1,  36,  37,
    38,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  49,  50,  51,  -1,  -1,  -1,  -1,  56,
    57,  58,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  69,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  109, 110, 111, 112, 113,
    114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 23,  24,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  32,  33,  34,  -1,  36,  37,  38,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  49,  50,  51,  -1,  -1,  -1,  -1,  56,  57,  58,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  69,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
    122, 123, 124, 125, 126, 127, 128, 3,   4,   5,   -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  22,  -1,  -1,  25,  26,  27,  28,  29,  30,  31,  -1,  -1,
    -1,  35,  -1,  -1,  -1,  -1,  -1,  41,  42,  43,  44,  45,  -1,  47,  48,  -1,  -1,  -1,  52,
    53,  54,  55,  -1,  -1,  -1,  59,  60,  61,  -1,  63,  64,  65,  66,  67,  68,  -1,  70,  71,
    72,  73,  74,  75,  76,  77,  78,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  87,  88,  89,  90,
    91,  92,  93,  -1,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 23,  24,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  32,  33,  34,  -1,  36,  37,  38,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  49,  50,  51,  -1,  -1,  -1,  -1,  56,  57,  58,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  69,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  85,  86,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  94,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 126, 6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,
    18,  19,  20,  21,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  40,  -1,  -1,  -1,  -1,  -1,  46,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  79,  80,  81,  82,  83,  84,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  108};

const short PipelineParserGen::yystos_[] = {
    0,   130, 131, 132, 278, 85,  174, 86,  269, 86,  272, 0,   86,  175, 270, 273, 79,  80,  81,
    82,  83,  84,  176, 177, 178, 179, 180, 181, 182, 39,  6,   7,   8,   9,   10,  11,  12,  13,
    14,  15,  16,  17,  18,  19,  20,  21,  40,  46,  108, 137, 138, 140, 141, 146, 3,   4,   5,
    22,  25,  26,  27,  28,  29,  30,  31,  35,  40,  41,  42,  43,  44,  45,  47,  48,  52,  53,
    54,  55,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  74,  75,  76,
    77,  78,  79,  80,  81,  82,  83,  84,  87,  88,  89,  90,  91,  92,  93,  95,  96,  97,  98,
    99,  100, 101, 102, 103, 104, 105, 106, 107, 136, 138, 139, 140, 141, 142, 277, 86,  32,  33,
    34,  36,  37,  38,  49,  50,  51,  56,  57,  58,  120, 121, 122, 123, 151, 153, 154, 155, 185,
    86,  86,  185, 279, 280, 40,  23,  24,  69,  85,  86,  94,  109, 110, 111, 112, 113, 114, 115,
    116, 117, 118, 119, 124, 125, 126, 127, 128, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156,
    157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 173, 271, 32,  33,
    36,  37,  49,  50,  56,  57,  86,  274, 275, 40,  183, 19,  7,   86,  175, 158, 267, 172, 62,
    40,  46,  134, 136, 138, 139, 140, 143, 185, 159, 267, 39,  40,  142, 145, 69,  94,  276, 23,
    24,  32,  33,  34,  36,  37,  38,  49,  50,  51,  56,  57,  58,  85,  86,  120, 121, 122, 123,
    147, 148, 149, 150, 152, 156, 157, 159, 161, 162, 163, 165, 166, 167, 184, 187, 189, 190, 192,
    193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211,
    213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231,
    232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250,
    251, 252, 253, 254, 255, 256, 257, 258, 259, 184, 40,  16,  158, 40,  168, 186, 187, 266, 3,
    4,   5,   22,  25,  26,  27,  28,  29,  30,  31,  35,  41,  42,  43,  44,  45,  47,  48,  52,
    53,  54,  55,  59,  60,  61,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  74,  75,  76,
    77,  78,  87,  88,  89,  90,  91,  92,  93,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 191, 154, 266, 39,  186, 189, 189, 85,  188, 186, 188, 85,  85,  279, 279, 279,
    85,  188, 186, 186, 188, 188, 85,  85,  85,  186, 85,  186, 188, 188, 279, 85,  85,  188, 85,
    189, 85,  212, 279, 212, 212, 279, 279, 85,  279, 85,  186, 85,  186, 186, 85,  85,  85,  85,
    186, 186, 186, 186, 186, 186, 186, 186, 186, 186, 279, 85,  186, 40,  135, 136, 138, 140, 141,
    144, 40,  40,  40,  40,  186, 40,  40,  40,  266, 158, 12,  9,   8,   186, 40,  40,  40,  40,
    40,  186, 186, 158, 40,  186, 40,  40,  40,  6,   264, 186, 186, 40,  186, 40,  186, 40,  12,
    40,  40,  10,  10,  186, 264, 186, 40,  186, 40,  40,  186, 186, 186, 186, 40,  40,  40,  40,
    40,  40,  40,  40,  40,  40,  264, 186, 40,  186, 186, 39,  39,  186, 186, 186, 186, 186, 186,
    39,  186, 186, 12,  186, 186, 39,  186, 186, 186, 186, 186, 12,  186, 186, 186, 186, 186, 186,
    12,  186, 39,  40,  40,  13,  260, 11,  262, 262, 39,  186, 268, 268, 40,  39,  186, 39,  266,
    40,  39,  15,  265, 12,  12,  39,  186, 39,  39,  186, 186, 186, 39,  186, 39,  186, 14,  261,
    186, 20,  263, 263, 40,  186, 39,  39,  40,  40,  40,  39,  40,  186, 17,  186, 186, 40,  40,
    40,  40,  39,  39,  39,  40,  40,  40,  186, 21,  186, 260, 261, 40,  40,  40,  40,  186, 18,
    18,  40,  40,  40,  40,  40,  186, 261, 40,  40,  186, 186, 40,  40,  40,  40,  40,  40,  40,
    40,  40};

const short PipelineParserGen::yyr1_[] = {
    0,   133, 278, 278, 278, 174, 175, 175, 280, 279, 176, 176, 176, 176, 176, 176, 182, 177, 178,
    185, 185, 185, 185, 179, 180, 181, 183, 183, 143, 143, 184, 184, 184, 184, 184, 184, 184, 184,
    184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184, 184,
    184, 184, 184, 184, 184, 184, 134, 134, 134, 134, 269, 270, 270, 146, 271, 137, 137, 137, 140,
    136, 136, 136, 136, 136, 136, 138, 138, 138, 138, 138, 138, 138, 138, 138, 138, 138, 138, 138,
    138, 138, 138, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139, 139,
    139, 139, 139, 139, 139, 139, 139, 139, 159, 159, 159, 160, 173, 161, 162, 163, 165, 166, 167,
    147, 148, 149, 150, 152, 156, 157, 151, 151, 151, 151, 153, 153, 153, 153, 154, 154, 154, 154,
    155, 155, 155, 155, 164, 164, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168, 168,
    168, 168, 168, 168, 168, 168, 168, 168, 266, 266, 186, 186, 188, 187, 187, 187, 187, 187, 187,
    187, 187, 189, 190, 191, 191, 144, 135, 135, 135, 135, 141, 192, 192, 192, 192, 192, 192, 192,
    192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 193, 194, 245, 246, 247, 248, 249, 250, 251,
    252, 253, 254, 255, 256, 257, 258, 259, 195, 195, 195, 196, 197, 198, 202, 202, 202, 202, 202,
    202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 202, 203, 262,
    262, 263, 263, 204, 205, 268, 268, 268, 206, 207, 264, 264, 208, 215, 225, 265, 265, 212, 209,
    210, 211, 213, 214, 216, 217, 218, 219, 220, 221, 222, 223, 224, 276, 276, 274, 272, 273, 273,
    275, 275, 275, 275, 275, 275, 275, 275, 277, 277, 199, 199, 200, 201, 158, 158, 169, 169, 170,
    267, 267, 171, 172, 172, 145, 142, 142, 142, 142, 142, 226, 226, 226, 226, 226, 226, 226, 227,
    228, 229, 230, 231, 232, 233, 234, 234, 234, 234, 234, 234, 234, 234, 234, 234, 260, 260, 261,
    261, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244};

const signed char PipelineParserGen::yyr2_[] = {
    0, 2, 2, 2, 2, 3, 0, 4,  0,  2, 1,  1, 1, 1, 1, 1, 5, 3, 7, 1, 1, 1, 1, 2, 2, 4,  0,  2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 3,  0,  2, 2,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 0,  2,  1, 1,  4, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 0, 2, 2, 1,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 7, 4, 4, 4, 7,  4,  7, 8, 7,
    7, 4, 7, 7, 1, 1, 1, 4,  4,  6, 1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1, 1, 1,
    1, 1, 6, 0, 2, 0, 2, 11, 10, 0, 1,  2, 8, 8, 0, 2, 8, 8, 8, 0, 2, 7, 4, 4, 4, 11, 11, 7, 4, 4,
    7, 8, 8, 8, 4, 4, 1, 1,  4,  3, 0,  2, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 6, 6,  1,  1, 1, 1,
    3, 0, 2, 3, 0, 2, 2, 1,  1,  1, 1,  1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4,  1,  1, 1, 1,
    1, 1, 1, 1, 1, 1, 0, 2,  0,  2, 11, 4, 4, 4, 4, 4, 4, 4, 4, 4};


#if YYDEBUG || 1
// YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
// First, the terminals, then, starting at \a YYNTOKENS, nonterminals.
const char* const PipelineParserGen::yytname_[] = {"\"EOF\"",
                                                   "error",
                                                   "\"invalid token\"",
                                                   "ABS",
                                                   "ADD",
                                                   "AND",
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
                                                   "START_PIPELINE",
                                                   "START_MATCH",
                                                   "START_SORT",
                                                   "$accept",
                                                   "projectionFieldname",
                                                   "expressionFieldname",
                                                   "stageAsUserFieldname",
                                                   "filterFieldname",
                                                   "argAsUserFieldname",
                                                   "aggExprAsUserFieldname",
                                                   "invariableUserFieldname",
                                                   "idAsUserFieldname",
                                                   "valueFieldname",
                                                   "projectField",
                                                   "expressionField",
                                                   "valueField",
                                                   "filterField",
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
                                                   "matchExpression",
                                                   "filterFields",
                                                   "filterVal",
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
const short PipelineParserGen::yyrline_[] = {
    0,    297,  297,  301,  305,  312,  318,  319,  327,  327,  330,  330,  330,  330,  330,  330,
    333,  343,  349,  359,  359,  359,  359,  363,  368,  373,  389,  392,  399,  402,  408,  409,
    410,  411,  412,  413,  414,  415,  416,  417,  418,  419,  422,  425,  428,  431,  434,  437,
    440,  443,  446,  449,  452,  455,  458,  461,  464,  467,  470,  473,  474,  475,  476,  485,
    485,  485,  485,  489,  495,  498,  504,  510,  515,  515,  515,  519,  527,  530,  533,  536,
    539,  542,  551,  554,  557,  560,  563,  566,  569,  572,  575,  578,  581,  584,  587,  590,
    593,  596,  604,  607,  610,  613,  616,  619,  622,  625,  628,  631,  634,  637,  640,  643,
    646,  649,  652,  655,  658,  661,  664,  667,  670,  673,  676,  679,  682,  685,  688,  691,
    694,  697,  700,  703,  706,  709,  712,  715,  718,  721,  724,  727,  730,  733,  736,  739,
    742,  745,  748,  751,  754,  757,  760,  763,  766,  769,  772,  775,  778,  781,  784,  787,
    794,  799,  802,  808,  816,  825,  831,  837,  843,  849,  855,  861,  867,  873,  879,  885,
    891,  897,  903,  906,  909,  912,  918,  921,  924,  927,  933,  936,  939,  942,  948,  951,
    954,  957,  963,  966,  972,  973,  974,  975,  976,  977,  978,  979,  980,  981,  982,  983,
    984,  985,  986,  987,  988,  989,  990,  991,  992,  999,  1000, 1007, 1007, 1011, 1016, 1016,
    1016, 1016, 1016, 1016, 1017, 1017, 1023, 1031, 1037, 1040, 1047, 1054, 1054, 1054, 1054, 1058,
    1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1064, 1065, 1065, 1065,
    1065, 1069, 1076, 1082, 1087, 1092, 1098, 1103, 1108, 1113, 1119, 1124, 1130, 1139, 1145, 1151,
    1156, 1162, 1168, 1168, 1168, 1172, 1179, 1186, 1193, 1193, 1193, 1193, 1193, 1193, 1193, 1194,
    1194, 1194, 1194, 1194, 1194, 1194, 1194, 1195, 1195, 1195, 1195, 1195, 1195, 1195, 1199, 1209,
    1212, 1218, 1221, 1227, 1236, 1245, 1248, 1251, 1257, 1268, 1279, 1282, 1288, 1296, 1304, 1312,
    1315, 1320, 1329, 1335, 1341, 1347, 1357, 1367, 1374, 1381, 1388, 1396, 1404, 1412, 1420, 1426,
    1432, 1435, 1441, 1447, 1452, 1455, 1462, 1465, 1468, 1471, 1474, 1477, 1480, 1483, 1488, 1490,
    1496, 1496, 1500, 1507, 1514, 1514, 1518, 1518, 1522, 1528, 1529, 1536, 1542, 1545, 1552, 1559,
    1560, 1561, 1562, 1563, 1566, 1566, 1566, 1566, 1566, 1566, 1566, 1568, 1573, 1578, 1583, 1588,
    1593, 1598, 1604, 1605, 1606, 1607, 1608, 1609, 1610, 1611, 1612, 1613, 1618, 1621, 1628, 1631,
    1637, 1647, 1652, 1657, 1662, 1667, 1672, 1677, 1682, 1687};

void PipelineParserGen::yy_stack_print_() const {
    *yycdebug_ << "Stack now";
    for (stack_type::const_iterator i = yystack_.begin(), i_end = yystack_.end(); i != i_end; ++i)
        *yycdebug_ << ' ' << int(i->state);
    *yycdebug_ << '\n';
}

void PipelineParserGen::yy_reduce_print_(int yyrule) const {
    int yylno = yyrline_[yyrule];
    int yynrhs = yyr2_[yyrule];
    // Print the symbols being reduced, and their result.
    *yycdebug_ << "Reducing stack by rule " << yyrule - 1 << " (line " << yylno << "):\n";
    // The symbols being reduced.
    for (int yyi = 0; yyi < yynrhs; yyi++)
        YY_SYMBOL_PRINT("   $" << yyi + 1 << " =", yystack_[(yynrhs) - (yyi + 1)]);
}
#endif  // YYDEBUG


#line 58 "pipeline_grammar.yy"
}  // namespace mongo
#line 5920 "pipeline_parser_gen.cpp"

#line 1691 "pipeline_grammar.yy"
