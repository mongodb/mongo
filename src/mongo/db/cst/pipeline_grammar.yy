/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

//
// This is a grammar file to describe the syntax of the aggregation pipeline language. It is 
// ingested by GNU Bison (https://www.gnu.org/software/bison/) to generate native C++ parser code
// based on the rules provided here.
// 
// To manually generate the parser files, run 
// 'bison pipeline_grammar.yy -o pipeline_parser_gen.cpp'.
// 
%require "3.5"
%language "c++"

// Generate header for tokens to be included from lexer.
%defines
// Tell Bison to generate make_* methods for tokens.
%define api.token.constructor
// The mapping of scanner token ID to Bison's internal symbol enum is consistent.
%define api.token.raw
// Instead of specifying a %union directive of possible semantic types, allow Bison to build a sort
// of std::variant structure. This allows symbol declaration with '%type <C++ type> symbol'.
%define api.value.type variant

// Every $foo becomes a std::move(*pull_foo_from_stack*). This makes the syntax cleaner and prevents
// accidental copies but each $foo must be used only once per production rule! Move foo into an auto
// variable first if multiple copies are needed. Manually writing std::move($foo) is harmless but
// should be avoided since it's redundant.
%define api.value.automove

%define parse.assert
%define api.namespace {mongo}
%define api.parser.class {PipelineParserGen}

// Track locations of symbols.
%locations
%define api.location.file "location_gen.h"

// Header only.
%code requires {
    #include "mongo/db/cst/c_node.h"
    #include "mongo/db/cst/key_fieldname.h"
    #include "mongo/stdx/variant.h"

    // Forward declare any parameters needed for lexing/parsing.
    namespace mongo {
        class BSONLexer;
    }

    #ifdef _MSC_VER
    // warning C4065: switch statement contains 'default' but no 'case' labels.
    #pragma warning (disable : 4065)
    #endif
}

// Cpp only.
%code { 
    #include "mongo/db/cst/bson_lexer.h"
    #include "mongo/platform/decimal128.h"

    namespace mongo {
        // Mandatory error function.
        void PipelineParserGen::error (const PipelineParserGen::location_type& loc, 
                                       const std::string& msg) {
            uasserted(ErrorCodes::FailedToParse, str::stream() << msg <<
                    " at location " <<
                    loc.begin.line << ":" << loc.begin.column <<
                    " of input BSON. Lexer produced token of type " <<
                    lexer[loc.begin.column].type_get() << "." );
        }
    }  // namespace mongo
}

// Parsing parameters, funneled through yyparse() to yylex().
%param {BSONLexer& lexer}
// yyparse() parameter only.
%parse-param {CNode* cst}

//
// Token definitions.
//

%token 
    START_OBJECT
    END_OBJECT
    START_ARRAY
    END_ARRAY

    ID

    // Special literals
    INT_ZERO
    LONG_ZERO
    DOUBLE_ZERO
    DECIMAL_ZERO
    BOOL_TRUE
    BOOL_FALSE

    // Reserve pipeline stage names.
    STAGE_INHIBIT_OPTIMIZATION
    STAGE_LIMIT
    STAGE_PROJECT
    STAGE_SAMPLE
    STAGE_SKIP
    STAGE_UNION_WITH
    STAGE_UNWIND

    // $unionWith arguments.
    COLL_ARG
    PIPELINE_ARG

    // $sample arguments.
    SIZE_ARG
    
    // $unwind arguments.
    PATH_ARG
    INCLUDE_ARRAY_INDEX_ARG
    PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG

    // Expressions
    ADD
    ATAN2
    AND
    CONST_EXPR
    LITERAL
    OR
    NOT
    CMP
    EQ
    GT
    GTE
    LT
    LTE
    NE
    CONVERT
    TO_BOOL
    TO_DATE
    TO_DECIMAL
    TO_DOUBLE
    TO_INT
    TO_LONG
    TO_OBJECT_ID
    TO_STRING
    TYPE
    ABS
    CEIL
    DIVIDE
    EXPONENT
    FLOOR
    LN
    LOG
    LOGTEN
    MOD
    MULTIPLY
    POW
    ROUND
    SQRT
    SUBTRACT
    TRUNC

    // $convert arguments.
    INPUT_ARG
    TO_ARG
    ON_ERROR_ARG
    ON_NULL_ARG

    END_OF_FILE 0 "EOF"
;

%token <std::string> FIELDNAME
%token <std::string> NONEMPTY_STRING
%token <BSONBinData> BINARY
%token <UserUndefined> UNDEFINED
%token <OID> OBJECT_ID
%token <Date_t> DATE_LITERAL
%token <UserNull> JSNULL
%token <BSONRegEx> REGEX
%token <BSONDBRef> DB_POINTER
%token <BSONCode> JAVASCRIPT
%token <BSONSymbol> SYMBOL
%token <BSONCodeWScope> JAVASCRIPT_W_SCOPE
%token <int> INT_NON_ZERO
%token <Timestamp> TIMESTAMP
%token <long long> LONG_NON_ZERO
%token <double> DOUBLE_NON_ZERO
%token <Decimal128> DECIMAL_NON_ZERO
%token <UserMinKey> MIN_KEY
%token <UserMaxKey> MAX_KEY
%token START_PIPELINE START_MATCH

// Special string literals.
%token <std::string> DOLLAR_STRING "a $-prefixed string"
%token <std::string> EMPTY_STRING "an empty string"

//
// Semantic values (aka the C++ types produced by the actions).
//

// Possible user fieldnames.
%nterm <CNode::Fieldname> projectionFieldname expressionFieldname stageAsUserFieldname filterFieldname
%nterm <CNode::Fieldname> argAsUserFieldname aggExprAsUserFieldname invariableUserFieldname
%nterm <CNode::Fieldname> idAsUserFieldname valueFieldname
%nterm <std::pair<CNode::Fieldname, CNode>> projectField expressionField valueField filterField

// Literals.
%nterm <CNode> dbPointer javascript symbol javascriptWScope int timestamp long double decimal 
%nterm <CNode> minKey maxKey value string binary undefined objectId bool date null regex
%nterm <CNode> simpleValue compoundValue valueArray valueObject valueFields
%nterm <CNode> dollarString nonDollarString

// Pipeline stages and related non-terminals.
%nterm <CNode> stageList stage inhibitOptimization unionWith skip limit project sample unwind
%nterm <CNode> projectFields projection num
%nterm <std::pair<CNode::Fieldname, CNode>> includeArrayIndexArg preserveNullAndEmptyArraysArg

// Aggregate expressions
%nterm <CNode> expression compoundExpression exprFixedTwoArg expressionArray expressionObject
%nterm <CNode> expressionFields maths add atan2 boolExps and or not literalEscapes const literal
%nterm <CNode> compExprs cmp eq gt gte lt lte ne
%nterm <CNode> typeExpression typeValue convert toBool toDate toDecimal toDouble toInt toLong
%nterm <CNode> toObjectId toString type
%nterm <CNode> abs ceil divide exponent floor ln log logten mod multiply pow round sqrt subtract trunc
%nterm <std::pair<CNode::Fieldname, CNode>> onErrorArg onNullArg
%nterm <std::vector<CNode>> expressions values
%nterm <CNode> matchExpression filterFields filterVal

%start start;
//
// Grammar rules
//
%%

start: 
    START_PIPELINE pipeline
    | START_MATCH matchExpression {
        *cst = CNode{$matchExpression};
    }
;

// Entry point to pipeline parsing.
pipeline:
    START_ARRAY stageList END_ARRAY {
        *cst = $stageList;
    }
;

stageList:
    %empty { }
    | START_OBJECT stage END_OBJECT stageList[stagesArg] { 
        $$ = CNode{CNode::ArrayChildren{$stage}};
    }
;

// Special rule to hint to the lexer that the next set of tokens should be sorted. Note that the 
// sort order is not lexicographical, but rather based on the enum generated from the %token list
// above.
START_ORDERED_OBJECT: { lexer.sortObjTokens(); } START_OBJECT;

stage:
    inhibitOptimization | unionWith | skip | limit | project | sample | unwind
;

unwind:
    STAGE_UNWIND dollarString {
       $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::unwind, $dollarString}}};
    }
    | STAGE_UNWIND START_ORDERED_OBJECT PATH_ARG dollarString includeArrayIndexArg 
        preserveNullAndEmptyArraysArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::unwind,
                CNode{CNode::ObjectChildren{
                    {KeyFieldname::pathArg, $dollarString},
                    $includeArrayIndexArg,
                    $preserveNullAndEmptyArraysArg
                }}
            }}};
    }
;

// Optional argument for $unwind.
includeArrayIndexArg:
    %empty {
        $$ = std::pair{KeyFieldname::includeArrayIndexArg, CNode{KeyValue::absentKey}};
    }
    | INCLUDE_ARRAY_INDEX_ARG nonDollarString {
        $$ = std::pair{KeyFieldname::includeArrayIndexArg, $nonDollarString};
    }
;

// Optional argument for $unwind.
preserveNullAndEmptyArraysArg:
    %empty {
        $$ = std::pair{KeyFieldname::preserveNullAndEmptyArraysArg, CNode{KeyValue::absentKey}};
    }
    | PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG bool {
        $$ = std::pair{KeyFieldname::preserveNullAndEmptyArraysArg, $bool};
    }
;

sample: STAGE_SAMPLE START_OBJECT SIZE_ARG num END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::sample, 
                CNode{CNode::ObjectChildren{
                    {KeyFieldname::sizeArg, $num},
                }}
            }}};
    }
;

inhibitOptimization:
    STAGE_INHIBIT_OPTIMIZATION START_OBJECT END_OBJECT { 
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
    }
;

unionWith:
    STAGE_UNION_WITH START_ORDERED_OBJECT COLL_ARG string PIPELINE_ARG double END_OBJECT {
    auto pipeline = $double;
    $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::unionWith,
        CNode{CNode::ObjectChildren{
            {KeyFieldname::collArg, $string},
            {KeyFieldname::pipelineArg, std::move(pipeline)}
     }}}}};
};

num:
   int | long | double | decimal
;

skip:
    STAGE_SKIP num {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::skip, $num}}};
};

limit:
    STAGE_LIMIT num {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::limit, $num}}};
};

project:
    STAGE_PROJECT START_OBJECT projectFields END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::project, $projectFields}}};
    }
;

projectFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | projectFields[projectArg] projectField {
        $$ = $projectArg;
        $$.objectChildren().emplace_back($projectField);
    }
;

projectField:
    ID projection {
        $$ = {KeyFieldname::id, $projection};
    }
    | projectionFieldname projection {
        $$ = {$projectionFieldname, $projection};
    }
;

projection:
    string
    | INT_NON_ZERO {
        $$ = CNode{NonZeroKey{$1}};
    }
    | INT_ZERO {
        $$ = CNode{KeyValue::intZeroKey};
    }
    | LONG_NON_ZERO {
        $$ = CNode{NonZeroKey{$1}};
    }
    | LONG_ZERO {
        $$ = CNode{KeyValue::longZeroKey};
    }
    | DOUBLE_NON_ZERO {
        $$ = CNode{NonZeroKey{$1}};
    }
    | DOUBLE_ZERO {
        $$ = CNode{KeyValue::doubleZeroKey};
    }
    | DECIMAL_NON_ZERO {
        $$ = CNode{NonZeroKey{$1}};
    }
    | DECIMAL_ZERO {
        $$ = CNode{KeyValue::decimalZeroKey};
    }
    | BOOL_TRUE {
        $$ = CNode{KeyValue::trueKey};
    }
    | BOOL_FALSE {
        $$ = CNode{KeyValue::falseKey};
    }
    | compoundExpression
;

projectionFieldname:
    invariableUserFieldname | stageAsUserFieldname | argAsUserFieldname | aggExprAsUserFieldname
;

matchExpression:
    START_OBJECT filterFields END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::match, $filterFields}}};
    }
;

filterFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | filterFields[filterArg] filterField {
        $$ = $filterArg;
        $$.objectChildren().emplace_back($filterField);
    }
;

filterField:
    ID filterVal {
        $$ = {KeyFieldname::id, $filterVal};
    }
    | filterFieldname filterVal {
        $$ = {$filterFieldname, $filterVal};
    }
;

filterVal:
    value
;

filterFieldname:
    invariableUserFieldname | stageAsUserFieldname | argAsUserFieldname | aggExprAsUserFieldname
;

invariableUserFieldname:
    FIELDNAME {
        $$ = UserFieldname{$1};
    }
;

stageAsUserFieldname:
    // Here we need to list all agg stage keys so they can be converted back to string in contexts
    // where they're not special. It's laborious but this is the perennial Bison way.
    STAGE_INHIBIT_OPTIMIZATION {
        $$ = UserFieldname{"$_internalInhibitOptimization"};
    }
    | STAGE_UNION_WITH {
        $$ = UserFieldname{"$unionWith"};
    }
    | STAGE_SKIP {
        $$ = UserFieldname{"$skip"};
    }
    | STAGE_LIMIT {
        $$ = UserFieldname{"$limit"};
    }
    | STAGE_PROJECT {
        $$ = UserFieldname{"$project"};
    }
    | STAGE_SAMPLE {
        $$ = UserFieldname{"$sample"};
    }
    | STAGE_UNWIND {
        $$ = UserFieldname{"$unwind"};
    }
;

argAsUserFieldname:
    // Here we need to list all keys representing args passed to operators so they can be converted
    // back to string in contexts where they're not special. It's laborious but this is the
    // perennial Bison way.
    COLL_ARG {
        $$ = UserFieldname{"coll"};
    }
    | PIPELINE_ARG {
        $$ = UserFieldname{"pipeline"};
    }
    | SIZE_ARG {
        $$ = UserFieldname{"size"};
    }
    | INPUT_ARG {
        $$ = UserFieldname{"input"};
    }
    | TO_ARG {
        $$ = UserFieldname{"to"};
    }
    | ON_ERROR_ARG {
        $$ = UserFieldname{"onError"};
    }
    | ON_NULL_ARG {
        $$ = UserFieldname{"onNull"};
    }
    | PATH_ARG {
        $$ = UserFieldname{"path"};
    }
    | INCLUDE_ARRAY_INDEX_ARG {
        $$ = UserFieldname{"includeArrayIndex"};
    }
    | PRESERVE_NULL_AND_EMPTY_ARRAYS_ARG {
        $$ = UserFieldname{"preserveNullAndEmptyArrays"};
    }
;

aggExprAsUserFieldname:
    // Here we need to list all agg expressions so they can be converted back to string in contexts
    // where they're not special. It's laborious but this is the perennial Bison way.
    ADD {
        $$ = UserFieldname{"$add"};
    }
    | ATAN2 {
        $$ = UserFieldname{"$atan2"};
    }
    | AND {
        $$ = UserFieldname{"$and"};
    }
    | CONST_EXPR {
        $$ = UserFieldname{"$const"};
    }
    | LITERAL {
        $$ = UserFieldname{"$literal"};
    }
    | OR {
        $$ = UserFieldname{"$or"};
    }
    | NOT {
        $$ = UserFieldname{"$not"};
    }
    | CMP {
        $$ = UserFieldname{"$cmp"};
    }
    | EQ {
        $$ = UserFieldname{"$eq"};
    }
    | GT {
        $$ = UserFieldname{"$gt"};
    }
    | GTE {
        $$ = UserFieldname{"$gte"};
    }
    | LT {
        $$ = UserFieldname{"$lt"};
    }
    | LTE {
        $$ = UserFieldname{"$lte"};
    }
    | NE {
        $$ = UserFieldname{"$ne"};
    }
    | CONVERT {
        $$ = UserFieldname{"$convert"};
    }
    | TO_BOOL {
        $$ = UserFieldname{"$toBool"};
    }
    | TO_DATE {
        $$ = UserFieldname{"$toDate"};
    }
    | TO_DECIMAL {
        $$ = UserFieldname{"$toDecimal"};
    }
    | TO_DOUBLE {
        $$ = UserFieldname{"$toDouble"};
    }
    | TO_INT {
        $$ = UserFieldname{"$toInt"};
    }
    | TO_LONG {
        $$ = UserFieldname{"$toLong"};
    }
    | TO_OBJECT_ID {
        $$ = UserFieldname{"$toObjectId"};
    }
    | TO_STRING {
        $$ = UserFieldname{"$toString"};
    }
    | TYPE {
        $$ = UserFieldname{"$type"};
    }
    | ABS {
        $$ = UserFieldname{"$abs"};
    }
    | CEIL {
        $$ = UserFieldname{"$ceil"};
    }
    | DIVIDE {
        $$ = UserFieldname{"$divide"};
    }
    | EXPONENT {
        $$ = UserFieldname{"$exp"};
    }
    | FLOOR {
        $$ = UserFieldname{"$floor"};
    }
    | LN {
        $$ = UserFieldname{"$ln"};
    }
    | LOG {
        $$ = UserFieldname{"$log"};
    }
    | LOGTEN {
        $$ = UserFieldname{"$log10"};
    }
    | MOD {
        $$ = UserFieldname{"$mod"};
    }
    | MULTIPLY {
        $$ = UserFieldname{"$multiply"};
    }
    | POW {
        $$ = UserFieldname{"$pow"};
    }
    | ROUND {
        $$ = UserFieldname{"$round"};
    }
    | SQRT {
       $$ = UserFieldname{"$sqrt"};
    }
    | SUBTRACT {
       $$ = UserFieldname{"$subtract"};
    }
    | TRUNC {
        $$ = UserFieldname{"$trunc"};
    }
;

// Rules for literal non-terminals. 
string:
    NONEMPTY_STRING {
        $$ = CNode{UserString{$1}};
    }
    | DOLLAR_STRING {
        $$ = CNode{UserString{$1}};
    }
    | EMPTY_STRING {
        $$ = CNode{UserString{$1}};
    }
;

// '$'-prefixed, non-empty strings.
dollarString:
    DOLLAR_STRING {
        $$ = CNode{UserString{$1}};
    }
;

// Non-'$'-prefixed, non-empty strings.
nonDollarString:
    NONEMPTY_STRING {
        $$ = CNode{UserString{$1}};
    }
;

binary:
    BINARY {
        $$ = CNode{UserBinary{$1}};
    }
;

undefined:
    UNDEFINED {
        $$ = CNode{UserUndefined{}};
    }
;

objectId:
    OBJECT_ID {
        $$ = CNode{UserObjectId{}};
    }
;

date:
    DATE_LITERAL {
        $$ = CNode{UserDate{$1}};
    }
;

null:
    JSNULL {
        $$ = CNode{UserNull{}};
    }
;

regex:
    REGEX {
        $$ = CNode{UserRegex{$1}};
    }
;

dbPointer:
    DB_POINTER {
        $$ = CNode{UserDBPointer{$1}};
    }
;

javascript:
    JAVASCRIPT {
        $$ = CNode{UserJavascript{$1}};
    }
;

symbol:
    SYMBOL {
        $$ = CNode{UserSymbol{$1}};
    }
;

javascriptWScope:
    JAVASCRIPT_W_SCOPE {
        $$ = CNode{UserJavascriptWithScope{$1}};
    }
;

timestamp:
    TIMESTAMP {
        $$ = CNode{UserTimestamp{$1}};
    }
;

minKey:
    MIN_KEY {
        $$ = CNode{UserMinKey{$1}};
    }
;

maxKey:
    MAX_KEY {
        $$ = CNode{UserMaxKey{$1}};
    }
;

int:
    INT_NON_ZERO {
        $$ = CNode{UserInt{$1}};
    }
    | INT_ZERO {
        $$ = CNode{UserInt{0}};
    }
;

long:
    LONG_NON_ZERO {
        $$ = CNode{UserLong{$1}};
    }
    | LONG_ZERO {
        $$ = CNode{UserLong{0ll}};
    }
;

double:
    DOUBLE_NON_ZERO {
        $$ = CNode{UserDouble{$1}};
    }
    | DOUBLE_ZERO {
        $$ = CNode{UserDouble{0.0}};
    }
;

decimal:
    DECIMAL_NON_ZERO {
        $$ = CNode{UserDecimal{$1}};
    }
    | DECIMAL_ZERO {
        $$ = CNode{UserDecimal{0.0}};
    }
;

bool:
    BOOL_TRUE {
        $$ = CNode{UserBoolean{true}};
    }
    | BOOL_FALSE {
        $$ = CNode{UserBoolean{false}};
    }
;

simpleValue:
    string
    | binary
    | undefined
    | objectId
    | date
    | null
    | regex
    | dbPointer
    | javascript
    | symbol
    | javascriptWScope
    | int
    | long
    | double
    | decimal
    | bool
    | timestamp
    | minKey
    | maxKey
;

// Zero or more expressions. Specify mandatory expressions in a rule using the 'expression'
// nonterminal and append this non-terminal if an unbounded number of additional optional
// expressions are allowed.
expressions:
    %empty { }
    | expression expressions[expressionArg] {
        $$ = $expressionArg;
        $$.emplace_back($expression);
    }
;

expression:
    simpleValue | compoundExpression
;

// Helper rule for expressions which take exactly two expression arguments.
exprFixedTwoArg: START_ARRAY expression[expr1] expression[expr2] END_ARRAY {
    $$ = CNode{CNode::ArrayChildren{$expr1, $expr2}};
};

compoundExpression:
    expressionArray | expressionObject | maths | boolExps | literalEscapes | compExprs
    | typeExpression
;

// These are arrays occuring in Expressions outside of $const/$literal. They may contain further
// Expressions.
expressionArray:
    START_ARRAY expressions END_ARRAY {
        $$ = CNode{$expressions};
    }
;

// These are objects occuring in Expressions outside of $const/$literal. They may contain further
// Expressions.
expressionObject:
    START_OBJECT expressionFields END_OBJECT {
        $$ = $expressionFields;
    }
;

expressionFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | expressionFields[expressionArg] expressionField {
        $$ = $expressionArg;
        $$.objectChildren().emplace_back($expressionField);
    }
;

expressionField:
    expressionFieldname expression {
        $$ = {$expressionFieldname, $expression};
    }
;

// All fieldnames that don't indicate agg functons/operators.
expressionFieldname:
    invariableUserFieldname | stageAsUserFieldname | argAsUserFieldname | idAsUserFieldname
;

idAsUserFieldname:
    ID {
        $$ = UserFieldname{"_id"};
    }
;

maths:
    add | atan2 | abs | ceil | divide | exponent | floor | ln | log | logten | mod | multiply | pow
| round | sqrt | subtract | trunc
;

add:
    START_OBJECT ADD START_ARRAY expression[expr1] expression[expr2] expressions END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::add,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

atan2:
    START_OBJECT ATAN2 exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::atan2,
                                          $exprFixedTwoArg}}};
    }
;
abs:
    START_OBJECT ABS expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::abs, $expression}}};
    }
;
ceil:
    START_OBJECT CEIL expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ceil, $expression}}};
    }
;
divide:
      START_OBJECT DIVIDE START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::divide,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
    }
;
exponent:
        START_OBJECT EXPONENT expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::exponent, $expression}}};
    }
;
floor:
     START_OBJECT FLOOR expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::floor, $expression}}};
    }
;
ln:
  START_OBJECT LN expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ln, $expression}}};
 }
;
log:
   START_OBJECT LOG START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::log,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
  }
;
logten:
      START_OBJECT LOGTEN expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::logten, $expression}}};
     }
;
mod:
   START_OBJECT MOD START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::mod,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
  }
;
multiply:
    START_OBJECT MULTIPLY START_ARRAY expression[expr1] expression[expr2] expressions END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::multiply,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;
pow:
   START_OBJECT POW START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::pow,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
  }
;
round:
   START_OBJECT ROUND START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::round,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
    }
;
sqrt:
      START_OBJECT SQRT expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::sqrt, $expression}}};
   }
;
subtract:
   START_OBJECT SUBTRACT START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::subtract,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
       }
;
trunc:
   START_OBJECT TRUNC START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::trunc,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
     }
;
boolExps:
    and | or | not
;

and:
    START_OBJECT AND START_ARRAY expression[expr1] expression[expr2] expressions END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::andExpr,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

or:
    START_OBJECT OR START_ARRAY expression[expr1] expression[expr2] expressions END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::orExpr,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

not:
    START_OBJECT NOT START_ARRAY expression END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::notExpr,
                                          CNode{CNode::ArrayChildren{$expression}}}}};
    }
;

literalEscapes:
    const | literal
;

const:
    START_OBJECT CONST_EXPR START_ARRAY value END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::constExpr,
                                          CNode{CNode::ArrayChildren{$value}}}}};
    }
;

literal:
    START_OBJECT LITERAL START_ARRAY value END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::literal,
                                          CNode{CNode::ArrayChildren{$value}}}}};
    }
;

value:
    simpleValue | compoundValue
;

compoundValue:
    valueArray | valueObject
;

valueArray:
    START_ARRAY values END_ARRAY {
        $$ = CNode{$values};
    }
;

values:
    %empty { }
    | value values[valuesArg] {
        $$ = $valuesArg;
        $$.emplace_back($value);
    }
;

valueObject:
    START_OBJECT valueFields END_OBJECT {
        $$ = $valueFields;
    }
;

valueFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | valueFields[valueArg] valueField {
        $$ = $valueArg;
        $$.objectChildren().emplace_back($valueField);
    }
;

valueField:
    valueFieldname value {
        $$ = {$valueFieldname, $value};
    }
;

// All fieldnames.
valueFieldname:
    invariableUserFieldname
    | stageAsUserFieldname
    | argAsUserFieldname
    | aggExprAsUserFieldname
    | idAsUserFieldname
;

compExprs: cmp | eq | gt | gte | lt | lte | ne;

cmp: START_OBJECT CMP exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::cmp,
                                          $exprFixedTwoArg}}};
};

eq: START_OBJECT EQ exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::eq,
                                          $exprFixedTwoArg}}};
};

gt: START_OBJECT GT exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::gt,
                                          $exprFixedTwoArg}}};
};

gte: START_OBJECT GTE exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::gte,
                                          $exprFixedTwoArg}}};
};

lt: START_OBJECT LT exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::lt,
                                          $exprFixedTwoArg}}};
};

lte: START_OBJECT LTE exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::lte,
                                          $exprFixedTwoArg}}};
};

ne: START_OBJECT NE exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ne,
                                          $exprFixedTwoArg}}};
};

typeExpression:
    convert
    | toBool
    | toDate
    | toDecimal
    | toDouble
    | toInt
    | toLong
    | toObjectId
    | toString
    | type
;

// Used in 'to' argument for $convert. Can be any valid expression that resolves to a string
// or numeric identifier of a BSON type.
typeValue:
    string | int | long | double | decimal;

// Optional argument for $convert.
onErrorArg:
    %empty {
        $$ = std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
    }
    | ON_ERROR_ARG expression {
        $$ = std::pair{KeyFieldname::onErrorArg, $expression};
    }
;

// Optional argument for $convert.
onNullArg:
    %empty {
        $$ = std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
    }
    | ON_NULL_ARG expression {
        $$ = std::pair{KeyFieldname::onNullArg, $expression};
    }
;

convert:
    START_OBJECT CONVERT START_ORDERED_OBJECT INPUT_ARG expression TO_ARG typeValue onErrorArg onNullArg END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::convert, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $expression},
                                         {KeyFieldname::toArg, $typeValue},
                                         $onErrorArg, $onNullArg}}}}};
    }
;

toBool:
    START_OBJECT TO_BOOL expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toBool, $expression}}};
    }

toDate:
    START_OBJECT TO_DATE expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toDate, $expression}}};
    }

toDecimal:
    START_OBJECT TO_DECIMAL expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, $expression}}};
    }

toDouble:
    START_OBJECT TO_DOUBLE expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, $expression}}};
    }

toInt:
    START_OBJECT TO_INT expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toInt, $expression}}};
    }

toLong:
    START_OBJECT TO_LONG expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toLong, $expression}}};
    }

toObjectId:
    START_OBJECT TO_OBJECT_ID expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toObjectId, $expression}}};
    }

toString:
    START_OBJECT TO_STRING expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toString, $expression}}};
    }

type:
    START_OBJECT TYPE expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::type, $expression}}};
    }

%%
