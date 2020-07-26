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
    STAGE_UNION_WITH
    STAGE_SKIP
    STAGE_LIMIT
    STAGE_PROJECT

    // $unionWith arguments.
    COLL_ARG
    PIPELINE_ARG

    // Expressions
    ADD
    ATAN2
    AND
    CONST
    LITERAL
    OR
    NOT
    

    END_OF_FILE 0 "EOF"
;

%token <std::string> FIELDNAME
%token <std::string> STRING
%token <BSONBinData> BINARY
%token <UserUndefined> UNDEFINED
%token <OID> OBJECT_ID
%token <Date_t> DATE
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

//
// Semantic values (aka the C++ types produced by the actions).
//
%nterm <CNode> stageList stage inhibitOptimization unionWith num skip limit project projectFields
%nterm <CNode> projection compoundExpression expressionArray expressionObject expressionFields
%nterm <CNode> expression maths add atan2 string binary undefined objectId bool date null regex
%nterm <CNode> dbPointer javascript symbol javascriptWScope int timestamp long double decimal minKey
%nterm <CNode> maxKey simpleValue boolExps and or not literalEscapes const literal value
%nterm <CNode> compoundValue valueArray valueObject valueFields
%nterm <CNode::Fieldname> projectionFieldname expressionFieldname stageAsUserFieldname
%nterm <CNode::Fieldname> argAsUserFieldname aggExprAsUserFieldname invariableUserFieldname
%nterm <CNode::Fieldname> idAsUserFieldname valueFieldname
%nterm <std::pair<CNode::Fieldname, CNode>> projectField expressionField valueField
%nterm <std::vector<CNode>> expressions values

//
// Grammar rules
//
%%

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
    inhibitOptimization | unionWith | skip | limit | project
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
        $skip = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::skip, $num}}};
};

limit:
    STAGE_LIMIT num {
        $limit = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::limit, $num}}};
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
    | CONST {
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
;

string:
    STRING {
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
    DATE {
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
        $$ = CNode{UserLong{0}};
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

compoundExpression:
    expressionArray | expressionObject | maths | boolExps | literalEscapes
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
    add
    | atan2
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
    START_OBJECT ATAN2 START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::atan2,
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
    START_OBJECT CONST START_ARRAY value END_ARRAY END_OBJECT {
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

%%
