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
// This is a grammar file to describe the syntax of the Mongo Query Language. It is
// ingested by GNU Bison (https://www.gnu.org/software/bison/) to generate native C++ parser code
// based on the rules provided here.
//
// To manually generate the parser files, run 'bison grammar.yy -o parser_gen.cpp'.
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
%define api.parser.class {ParserGen}

// Track locations of symbols.
%locations
%define api.location.type {mongo::BSONLocation}
%define parse.error verbose

// Header only.
%code requires {
    #include "mongo/db/cst/bson_location.h"
    #include "mongo/db/cst/c_node.h"

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
    #include "mongo/db/cst/c_node_disambiguation.h"
    #include "mongo/db/cst/c_node_validation.h"
    #include "mongo/db/cst/key_fieldname.h"
    #include "mongo/platform/decimal128.h"
    #include "mongo/stdx/variant.h"

    namespace mongo {
        // Mandatory error function.
        void ParserGen::error (const ParserGen::location_type& loc,
                                       const std::string& msg) {
            uasserted(ErrorCodes::FailedToParse, str::stream() << msg << " at element " << loc);
        }
    }  // namespace mongo

    // Default location for actions, called each time a rule is matched but before the action is
    // run. Also called when bison encounters a syntax ambiguity, which should not be relevant for
    // mongo.
    #define YYLLOC_DEFAULT(newPos, rhsPositions, nRhs)
}

// Parsing parameters, funneled through yyparse() to yylex().
%param {BSONLexer& lexer}
// yyparse() parameter only.
%parse-param {CNode* cst}

//
// Token definitions.
//

// If adding to this list, keep in alphabetical order since some rules expect a strict sorted order
// of tokens based on their enum value. The appended strings are used to generate user-friendly
// error messages.
%token
    ABS
    ADD
    AND
    ARG_CHARS "chars argument"
    ARG_COLL "coll argument"
    ARG_DATE "date argument"
    ARG_DATE_STRING "dateString argument"
    ARG_FILTER "filter"
    ARG_FIND "find argument"
    ARG_FORMAT "format argument"
    ARG_INPUT "input argument"
    ARG_ON_ERROR "onError argument"
    ARG_ON_NULL "onNull argument"
    ARG_OPTIONS "options argument"
    ARG_PIPELINE "pipeline argument"
    ARG_Q "q"
    ARG_QUERY "query"
    ARG_REGEX "regex argument"
    ARG_REPLACEMENT "replacement argument"
    ARG_SIZE "size argument"
    ARG_SORT "sort argument"
    ARG_TIMEZONE "timezone argument"
    ARG_TO "to argument"
    ATAN2
    BOOL_FALSE "false"
    BOOL_TRUE "true"
    CEIL
    CMP
    CONCAT
    CONST_EXPR
    CONVERT
    DATE_FROM_STRING
    DATE_TO_STRING
    DECIMAL_NEGATIVE_ONE "-1 (decimal)"
    DECIMAL_ONE "1 (decimal)"
    DECIMAL_ZERO "zero (decimal)"
    DIVIDE
    DOUBLE_NEGATIVE_ONE "-1 (double)"
    DOUBLE_ONE "1 (double)"
    DOUBLE_ZERO "zero (double)"
    END_ARRAY "end of array"
    END_OBJECT "end of object"
    EQ
    EXPONENT
    FLOOR
    GT
    GTE
    ID
    INDEX_OF_BYTES
    INDEX_OF_CP
    INT_NEGATIVE_ONE "-1 (int)"
    INT_ONE "1 (int)"
    INT_ZERO "zero (int)"
    LITERAL
    LN
    LOG
    LOGTEN
    LONG_NEGATIVE_ONE "-1 (long)"
    LONG_ONE "1 (long)"
    LONG_ZERO "zero (long)"
    LT
    LTE
    LTRIM
    META
    MOD
    MULTIPLY
    NE
    NOR
    NOT
    OR
    POW
    RAND_VAL "randVal"
    REGEX_FIND
    REGEX_FIND_ALL
    REGEX_MATCH
    REPLACE_ALL
    REPLACE_ONE
    ROUND
    RTRIM
    SPLIT
    SQRT
    STAGE_INHIBIT_OPTIMIZATION
    STAGE_LIMIT
    STAGE_PROJECT
    STAGE_SAMPLE
    STAGE_SKIP
    STAGE_UNION_WITH
    START_ARRAY "array"
    START_OBJECT "object"
    STR_CASE_CMP
    STR_LEN_BYTES
    STR_LEN_CP
    SUBSTR
    SUBSTR_BYTES
    SUBSTR_CP
    SUBTRACT
    TEXT_SCORE "textScore"
    TO_BOOL
    TO_DATE
    TO_DECIMAL
    TO_DOUBLE
    TO_INT
    TO_LONG
    TO_LOWER
    TO_OBJECT_ID
    TO_STRING
    TO_UPPER
    TRIM
    TRUNC
    TYPE

    END_OF_FILE 0 "EOF"
;

%token <std::string> FIELDNAME "fieldname"
%token <std::string> STRING "string"
%token <BSONBinData> BINARY "BinData"
%token <UserUndefined> UNDEFINED "undefined"
%token <OID> OBJECT_ID "ObjectID"
%token <Date_t> DATE_LITERAL "Date"
%token <UserNull> JSNULL "null"
%token <BSONRegEx> REGEX "regex"
%token <BSONDBRef> DB_POINTER "dbPointer"
%token <BSONCode> JAVASCRIPT "Code"
%token <BSONSymbol> SYMBOL "Symbol"
%token <BSONCodeWScope> JAVASCRIPT_W_SCOPE "CodeWScope"
%token <int> INT_OTHER "arbitrary integer"
%token <long long> LONG_OTHER "arbitrary long"
%token <double> DOUBLE_OTHER "arbitrary double"
%token <Decimal128> DECIMAL_OTHER "arbitrary decimal"
%token <Timestamp> TIMESTAMP "Timestamp"
%token <UserMinKey> MIN_KEY "minKey"
%token <UserMaxKey> MAX_KEY "maxKey"
%token <std::string> DOLLAR_STRING "$-prefixed string"
%token <std::string> DOLLAR_DOLLAR_STRING "$$-prefixed string"
%token <std::string> DOLLAR_PREF_FIELDNAME "$-prefixed fieldname"

//
// Semantic values (aka the C++ types produced by the actions).
//

// Possible user fieldnames.
%nterm <CNode::Fieldname> projectionFieldname expressionFieldname stageAsUserFieldname predFieldname
%nterm <CNode::Fieldname> argAsUserFieldname aggExprAsUserFieldname invariableUserFieldname
%nterm <CNode::Fieldname> idAsUserFieldname valueFieldname
%nterm <std::pair<CNode::Fieldname, CNode>> projectField expressionField valueField

// Literals.
%nterm <CNode> dbPointer javascript symbol javascriptWScope int timestamp long double decimal
%nterm <CNode> minKey maxKey value string fieldPath binary undefined objectId bool date null regex
%nterm <CNode> simpleValue compoundValue valueArray valueObject valueFields variable

// Pipeline stages and related non-terminals.
%nterm <CNode> pipeline stageList stage inhibitOptimization unionWith skip limit project sample
%nterm <CNode> projectFields projection num

// Aggregate expressions
%nterm <CNode> expression compoundExpression exprFixedTwoArg expressionArray expressionObject
%nterm <CNode> expressionFields maths add atan2 boolExps and or not literalEscapes const literal
%nterm <CNode> stringExps concat dateFromString dateToString indexOfBytes indexOfCP
%nterm <CNode> ltrim regexFind regexFindAll regexMatch regexArgs replaceOne replaceAll rtrim
%nterm <CNode> split strLenBytes strLenCP strcasecmp substr substrBytes substrCP
%nterm <CNode> toLower toUpper trim
%nterm <CNode> compExprs cmp eq gt gte lt lte ne
%nterm <CNode> typeExpression convert toBool toDate toDecimal toDouble toInt toLong
%nterm <CNode> toObjectId toString type
%nterm <CNode> abs ceil divide exponent floor ln log logten mod multiply pow round sqrt subtract trunc
%nterm <std::pair<CNode::Fieldname, CNode>> onErrorArg onNullArg
%nterm <std::pair<CNode::Fieldname, CNode>> formatArg timezoneArg charsArg optionsArg
%nterm <std::vector<CNode>> expressions values exprZeroToTwo

// Match expressions.
%nterm <CNode> match predicates compoundMatchExprs predValue additionalExprs
%nterm <std::pair<CNode::Fieldname, CNode>> predicate logicalExpr operatorExpression notExpr
%nterm <CNode::Fieldname> logicalExprField

// Sort related rules
%nterm <CNode> sortSpecs specList metaSort oneOrNegOne metaSortKeyword
%nterm <std::pair<CNode::Fieldname, CNode>> sortSpec

%start start;
//
// Grammar rules
//
%%

start:
    ARG_PIPELINE pipeline {
        *cst = $pipeline;
    }
    | ARG_FILTER match {
        *cst = $match;
    }
    | ARG_QUERY match {
        *cst = $match;
    }
    | ARG_Q match {
        *cst = $match;
    }
    | ARG_SORT sortSpecs {
        *cst = $sortSpecs;
    }
;

// Entry point to pipeline parsing.
pipeline:
    START_ARRAY stageList END_ARRAY {
        $$ = $stageList;
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
    inhibitOptimization | unionWith | skip | limit | project | sample
;

sample: STAGE_SAMPLE START_OBJECT ARG_SIZE num END_OBJECT {
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
    STAGE_UNION_WITH START_ORDERED_OBJECT ARG_COLL string ARG_PIPELINE double END_OBJECT {
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
        auto&& fields = $projectFields;
        if (auto inclusion = c_node_validation::validateProjectionAsInclusionOrExclusion(fields);
            inclusion.isOK())
            $$ = CNode{CNode::ObjectChildren{std::pair{inclusion.getValue() ==
                                                       c_node_validation::IsInclusion::yes ?
                                                       KeyFieldname::projectInclusion :
                                                       KeyFieldname::projectExclusion,
                                                       std::move(fields)}}};
        else
            // Pass the location of the $project token to the error reporting function.
            error(@1, inclusion.getStatus().reason());
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
    | INT_ONE {
        $$ = CNode{NonZeroKey{1}};
    }
    | INT_NEGATIVE_ONE {
        $$ = CNode{NonZeroKey{-1}};
    }
    | INT_OTHER {
        $$ = CNode{NonZeroKey{$1}};
    }
    | INT_ZERO {
        $$ = CNode{KeyValue::intZeroKey};
    }
    | LONG_ONE {
        $$ = CNode{NonZeroKey{1ll}};
    }
    | LONG_NEGATIVE_ONE {
        $$ = CNode{NonZeroKey{-1ll}};
    }
    | LONG_OTHER {
        $$ = CNode{NonZeroKey{$1}};
    }
    | LONG_ZERO {
        $$ = CNode{KeyValue::longZeroKey};
    }
    | DOUBLE_ONE {
        $$ = CNode{NonZeroKey{1.0}};
    }
    | DOUBLE_NEGATIVE_ONE {
        $$ = CNode{NonZeroKey{-1.0}};
    }
    | DOUBLE_OTHER {
        $$ = CNode{NonZeroKey{$1}};
    }
    | DOUBLE_ZERO {
        $$ = CNode{KeyValue::doubleZeroKey};
    }
    | DECIMAL_ONE {
        $$ = CNode{NonZeroKey{1.0}};
    }
    | DECIMAL_NEGATIVE_ONE {
        $$ = CNode{NonZeroKey{-1.0}};
    }
    | DECIMAL_OTHER {
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
    | timestamp
    | minKey
    | maxKey
    | compoundExpression {
        $$ = c_node_disambiguation::disambiguateCompoundProjection($1);
        if (stdx::holds_alternative<CompoundInconsistentKey>($$.payload))
            // TODO SERVER-50498: error() instead of uasserting
            uasserted(ErrorCodes::FailedToParse, "object project field cannot contain both inclusion and exclusion indicators");
    }
;

projectionFieldname:
    invariableUserFieldname | stageAsUserFieldname | argAsUserFieldname | aggExprAsUserFieldname
;

match:
    START_OBJECT predicates END_OBJECT {
        $$ = $predicates;
    }
;

predicates:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | predicates[filterArg] predicate {
        $$ = $filterArg;
        $$.objectChildren().emplace_back($predicate);
    }
;

predicate: predFieldname predValue {
        $$ = {$predFieldname, $predValue};
    }
    | logicalExpr {
        $$ = $logicalExpr;
    }
;

// TODO SERVER-48847: This rule assumes that object predicates always contain sub-expressions.
// Will need to expand to allow comparisons against literal objects (note that order of fields
// in object predicates is important! --> {a: 1, $gt: 2} is different than {$gt: 2, a: 1}).
predValue:
    simpleValue 
    | START_OBJECT compoundMatchExprs END_OBJECT {
        $$ = $compoundMatchExprs;
    }
;

compoundMatchExprs: 
    %empty {
        $$ = CNode::noopLeaf();
    }
    | compoundMatchExprs[exprs] operatorExpression {
        $$ = $exprs;
        $$.objectChildren().emplace_back($operatorExpression);
    }
;

// Rules for the operators which act on a path.
operatorExpression: notExpr

notExpr:
    NOT regex {
        $$ = std::pair{KeyFieldname::notExpr, $regex};
    }
    // $not requires an object with atleast one expression.
    | NOT START_OBJECT operatorExpression compoundMatchExprs END_OBJECT {
        auto&& exprs = $compoundMatchExprs;
        exprs.objectChildren().emplace_back($operatorExpression);

        $$ = std::pair{KeyFieldname::notExpr, std::move(exprs)};
    }
;

// Logical expressions accept an array of objects, with at least one element.
logicalExpr: logicalExprField START_ARRAY match additionalExprs END_ARRAY {
        auto&& children = $additionalExprs;
        children.arrayChildren().emplace_back($match);
        $$ = {$logicalExprField, std::move(children)};
    }
;

logicalExprField: 
    AND { $$ = KeyFieldname::andExpr; }
    | OR { $$ = KeyFieldname::orExpr; }
    | NOR { $$ = KeyFieldname::norExpr; }

additionalExprs: 
    %empty {
        $$ = CNode{CNode::ArrayChildren{}};
    }
    | additionalExprs[exprs] match {
        $$ = $exprs;
        $$.arrayChildren().emplace_back($match);
    }
;

// Filter predicates are *not* allowed over $-prefixed field names.
predFieldname: idAsUserFieldname | argAsUserFieldname | invariableUserFieldname;

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
;

argAsUserFieldname:
    // Here we need to list all keys representing args passed to operators so they can be converted
    // back to string in contexts where they're not special. It's laborious but this is the
    // perennial Bison way.
    ARG_COLL {
        $$ = UserFieldname{"coll"};
    }
    | ARG_PIPELINE {
        $$ = UserFieldname{"pipeline"};
    }
    | ARG_SIZE {
        $$ = UserFieldname{"size"};
    }
    | ARG_INPUT {
        $$ = UserFieldname{"input"};
    }
    | ARG_TO {
        $$ = UserFieldname{"to"};
    }
    | ARG_ON_ERROR {
        $$ = UserFieldname{"onError"};
    }
    | ARG_ON_NULL {
        $$ = UserFieldname{"onNull"};
    }
    | ARG_DATE_STRING {
        $$ = UserFieldname{"dateString"};
    }
    | ARG_FORMAT {
        $$ = UserFieldname{"format"};
    }
    | ARG_TIMEZONE {
        $$ = UserFieldname{"timezone"};
    }
    | ARG_DATE {
        $$ = UserFieldname{"date"};
    }
    | ARG_CHARS {
        $$ = UserFieldname{"chars"};
    }
    | ARG_REGEX {
        $$ = UserFieldname{"regex"};
    }
    | ARG_OPTIONS {
        $$ = UserFieldname{"options"};
    }
    | ARG_FIND {
        $$ = UserFieldname{"find"};
    }
    | ARG_REPLACEMENT {
        $$ = UserFieldname{"replacement"};
    }
    | ARG_FILTER {
        $$ = UserFieldname{"filter"};
    }
    | ARG_Q {
        $$ = UserFieldname{"q"};
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
    | CONCAT {
        $$ = UserFieldname{"$concat"};
    }
    | DATE_FROM_STRING {
        $$ = UserFieldname{"$dateFromString"};
    }
    | DATE_TO_STRING {
        $$ = UserFieldname{"$dateToString"};
    }
    | INDEX_OF_BYTES {
        $$ = UserFieldname{"$indexOfBytes"};
    }
    | INDEX_OF_CP {
        $$ = UserFieldname{"$indexOfCP"};
    }
    | LTRIM {
        $$ = UserFieldname{"$ltrim"};
    }
    | META {
        $$ = UserFieldname{"$meta"};
    }
    | REGEX_FIND {
        $$ = UserFieldname{"$regexFind"};
    }
    | REGEX_FIND_ALL {
        $$ = UserFieldname{"$regexFindAll"};
    }
    | REGEX_MATCH {
        $$ = UserFieldname{"$regexMatch"};
    }
    | REPLACE_ONE {
        $$ = UserFieldname{"$replaceOne"};
    }
    | REPLACE_ALL {
        $$ = UserFieldname{"$replaceAll"};
    }
    | RTRIM {
        $$ = UserFieldname{"$rtrim"};
    }
    | SPLIT {
        $$ = UserFieldname{"$split"};
    }
    | STR_LEN_BYTES {
        $$ = UserFieldname{"$strLenBytes"};
    }
    | STR_LEN_CP {
        $$ = UserFieldname{"$strLenCP"};
    }
    | STR_CASE_CMP {
        $$ = UserFieldname{"$strcasecmp"};
    }
    | SUBSTR {
        $$ = UserFieldname{"$substr"};
    }
    | SUBSTR_BYTES {
        $$ = UserFieldname{"$substrBytes"};
    }
    | SUBSTR_CP {
        $$ = UserFieldname{"$substrCP"};
    }
    | TO_LOWER {
        $$ = UserFieldname{"$toLower"};
    }
    | TRIM {
        $$ = UserFieldname{"$trim"};
    }
    | TO_UPPER {
        $$ = UserFieldname{"$toUpper"};
    }
;

// Rules for literal non-terminals.
string:
    STRING {
        $$ = CNode{UserString{$1}};
    }
    // Here we need to list all keys in value BSON positions so they can be converted back to string
    // in contexts where they're not special. It's laborious but this is the perennial Bison way.
    | RAND_VAL {
        $$ = CNode{UserString{"randVal"}};
    }
    | TEXT_SCORE {
        $$ = CNode{UserString{"textScore"}};
    }
;

fieldPath:
    DOLLAR_STRING {
        std::string str = $1;
        if (str.size() == 1) {
            error(@1, "'$' by iteslf is not a valid FieldPath");
        }
        $$ = CNode{UserFieldPath{str.substr(1), false}};
    }
variable:
    DOLLAR_DOLLAR_STRING {
        std::string str = $1.substr(2);
        auto status = c_node_validation::validateVariableName(str);
        if (!status.isOK()) {
            error(@1, status.reason());
        }
        $$ = CNode{UserFieldPath{str, true}};
    }
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
    INT_OTHER {
        $$ = CNode{UserInt{$1}};
    }
    | INT_ZERO {
        $$ = CNode{UserInt{0}};
    }
    | INT_ONE {
        $$ = CNode{UserInt{1}};
    }
    | INT_NEGATIVE_ONE {
        $$ = CNode{UserInt{-1}};
    }
;

long:
    LONG_OTHER {
        $$ = CNode{UserLong{$1}};
    }
    | LONG_ZERO {
        $$ = CNode{UserLong{0ll}};
    }
    | LONG_ONE {
        $$ = CNode{UserLong{1ll}};
    }
    | LONG_NEGATIVE_ONE {
        $$ = CNode{UserLong{-1ll}};
    }
;

double:
    DOUBLE_OTHER {
        $$ = CNode{UserDouble{$1}};
    }
    | DOUBLE_ZERO {
        $$ = CNode{UserDouble{0.0}};
    }
    | DOUBLE_ONE {
        $$ = CNode{UserDouble{1.0}};
    }
    | DOUBLE_NEGATIVE_ONE {
        $$ = CNode{UserDouble{-1.0}};
    }
;

decimal:
    DECIMAL_OTHER {
        $$ = CNode{UserDecimal{$1}};
    }
    | DECIMAL_ZERO {
        $$ = CNode{UserDecimal{0.0}};
    }
    | DECIMAL_ONE {
        $$ = CNode{UserDecimal{1.0}};
    }
    | DECIMAL_NEGATIVE_ONE {
        $$ = CNode{UserDecimal{-1.0}};
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
    | fieldPath
    | variable
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
    | typeExpression | stringExps
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
    START_OBJECT ADD expressionArray END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::add,
                                          $expressionArray}}};
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
    START_OBJECT AND expressionArray END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::andExpr,
                                          $expressionArray}}};
    }
;

or:
    START_OBJECT OR expressionArray END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::orExpr,
                                          $expressionArray}}};
    }
;

not:
    START_OBJECT NOT START_ARRAY expression END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::notExpr,
                                          CNode{CNode::ArrayChildren{$expression}}}}};
    }
;

stringExps:
    concat | dateFromString | dateToString | indexOfBytes | indexOfCP | ltrim | regexFind
    | regexFindAll | regexMatch | replaceOne | replaceAll | rtrim | split | strLenBytes | strLenCP
    | strcasecmp | substr | substrBytes | substrCP | toLower | trim | toUpper
;

concat:
    START_OBJECT CONCAT START_ARRAY expressions END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::concat,
                                          CNode{CNode::ArrayChildren{}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

formatArg:
    %empty {
        $$ = std::pair{KeyFieldname::formatArg, CNode{KeyValue::absentKey}};
    }
    | ARG_FORMAT expression {
        $$ = std::pair{KeyFieldname::formatArg, $expression};
    }
;

timezoneArg:
    %empty {
        $$ = std::pair{KeyFieldname::timezoneArg, CNode{KeyValue::absentKey}};
    }
    | ARG_TIMEZONE expression {
        $$ = std::pair{KeyFieldname::timezoneArg, $expression};
    }
;

dateFromString:
    START_OBJECT DATE_FROM_STRING START_ORDERED_OBJECT ARG_DATE_STRING expression formatArg timezoneArg
            onErrorArg onNullArg END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dateFromString, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::dateStringArg, $expression},
                                         $formatArg, $timezoneArg, $onErrorArg, $onNullArg}}}}};
    }
;

dateToString:
    START_OBJECT DATE_TO_STRING START_ORDERED_OBJECT ARG_DATE expression formatArg timezoneArg onNullArg
            END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dateToString, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::dateArg, $expression},
                                         $formatArg, $timezoneArg, $onNullArg}}}}};
    }
;

exprZeroToTwo:
    %empty {
        $$ = CNode::ArrayChildren{};
    }
    | expression {
        $$ = CNode::ArrayChildren{$expression};
    }
    | expression[expr1] expression[expr2] {
        $$ = CNode::ArrayChildren{$expr1, $expr2};
    }
;

indexOfBytes:
    START_OBJECT INDEX_OF_BYTES START_ARRAY expression[expr1] expression[expr2] exprZeroToTwo
            END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::indexOfBytes,
                    CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $exprZeroToTwo;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

indexOfCP:
    START_OBJECT INDEX_OF_CP START_ARRAY expression[expr1] expression[expr2] exprZeroToTwo
            END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::indexOfCP,
                    CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $exprZeroToTwo;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

charsArg:
    %empty {
        $$ = std::pair{KeyFieldname::charsArg, CNode{KeyValue::absentKey}};
    }
    | ARG_CHARS expression {
        $$ = std::pair{KeyFieldname::charsArg, $expression};
    }
;

ltrim:
    START_OBJECT LTRIM START_ORDERED_OBJECT charsArg ARG_INPUT expression END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ltrim, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $expression},
                                         $charsArg}}}}};
    }
;

rtrim:
    START_OBJECT RTRIM START_ORDERED_OBJECT charsArg ARG_INPUT expression END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::rtrim, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $expression},
                                         $charsArg}}}}};
    }
;

trim:
    START_OBJECT TRIM START_ORDERED_OBJECT charsArg ARG_INPUT expression END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::trim, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $expression},
                                         $charsArg}}}}};
    }
;

optionsArg:
    %empty {
        $$ = std::pair{KeyFieldname::optionsArg, CNode{KeyValue::absentKey}};
    }
    | ARG_OPTIONS expression {
        $$ = std::pair{KeyFieldname::optionsArg, $expression};
    }
;

regexArgs: START_ORDERED_OBJECT ARG_INPUT expression[input] optionsArg ARG_REGEX expression[regex] END_OBJECT {
    // Note that the order of these arguments must match the constructor for the regex expression.
    $$ = CNode{CNode::ObjectChildren{
                 {KeyFieldname::inputArg, $input},
                 {KeyFieldname::regexArg, $regex},
                 $optionsArg}};
};

regexFind:
    START_OBJECT REGEX_FIND regexArgs END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::regexFind, $regexArgs}}};
    }
;

regexFindAll:
    START_OBJECT REGEX_FIND_ALL regexArgs END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::regexFindAll, $regexArgs}}};
    }
;

regexMatch:
    START_OBJECT REGEX_MATCH regexArgs END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::regexMatch, $regexArgs}}};
    }
;

replaceOne:
    START_OBJECT REPLACE_ONE START_ORDERED_OBJECT ARG_FIND expression[find] ARG_INPUT expression[input]
        ARG_REPLACEMENT expression[replace] END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::replaceOne, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $input},
                                         {KeyFieldname::findArg, $find},
                                         {KeyFieldname::replacementArg, $replace}}}}}};
    }
;

replaceAll:
    START_OBJECT REPLACE_ALL START_ORDERED_OBJECT ARG_FIND expression[find] ARG_INPUT expression[input]
        ARG_REPLACEMENT expression[replace] END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::replaceAll, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $input},
                                         {KeyFieldname::findArg, $find},
                                         {KeyFieldname::replacementArg, $replace}}}}}};
    }
;

split:
    START_OBJECT SPLIT START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::split,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
    }
;

strLenBytes:
    START_OBJECT STR_LEN_BYTES expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::strLenBytes,
                                          $expression}}};
    }
;

strLenCP:
    START_OBJECT STR_LEN_CP expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::strLenCP,
                                          $expression}}};
    }
;

strcasecmp:
    START_OBJECT STR_CASE_CMP START_ARRAY expression[expr1] expression[expr2]
            END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::strcasecmp,
                    CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
    }
;

substr:
    START_OBJECT SUBSTR START_ARRAY expression[expr1] expression[expr2]
            expression[expr3] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::substr,
                    CNode{CNode::ArrayChildren{$expr1, $expr2, $expr3}}}}};
    }
;

substrBytes:
    START_OBJECT SUBSTR_BYTES START_ARRAY expression[expr1] expression[expr2]
            expression[expr3] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::substrBytes,
                    CNode{CNode::ArrayChildren{$expr1, $expr2, $expr3}}}}};
    }
;

substrCP:
    START_OBJECT SUBSTR_CP START_ARRAY expression[expr1] expression[expr2]
            expression[expr3] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::substrCP,
                    CNode{CNode::ArrayChildren{$expr1, $expr2, $expr3}}}}};
    }
;

toLower:
    START_OBJECT TO_LOWER expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toLower, $expression}}};
    }
;

toUpper:
    START_OBJECT TO_UPPER expression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::toUpper, $expression}}};
    }
;

metaSortKeyword:
    RAND_VAL {
        $$ = CNode{KeyValue::randVal};
    }
    | TEXT_SCORE {
        $$ = CNode{KeyValue::textScore};
    }
;

metaSort:
    START_OBJECT META metaSortKeyword END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, $metaSortKeyword}}};
}
;

sortSpecs:
    START_OBJECT specList END_OBJECT {
        $$ = $2;
}

specList:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | specList[sortArg] sortSpec {
        $$ = $sortArg;
        $$.objectChildren().emplace_back($sortSpec);
    }
;

oneOrNegOne:
    INT_ONE {
        $$ = CNode{KeyValue::intOneKey};
    }
    | INT_NEGATIVE_ONE {
        $$ = CNode{KeyValue::intNegOneKey};
    }
    | LONG_ONE {
        $$ = CNode{KeyValue::longOneKey};
    }
    | LONG_NEGATIVE_ONE {
        $$ = CNode{KeyValue::longNegOneKey};
    }
    | DOUBLE_ONE {
        $$ = CNode{KeyValue::doubleOneKey};
    }
    | DOUBLE_NEGATIVE_ONE {
        $$ = CNode{KeyValue::doubleNegOneKey};
    }
    | DECIMAL_ONE {
        $$ = CNode{KeyValue::decimalOneKey};
    }
    | DECIMAL_NEGATIVE_ONE {
        $$ = CNode{KeyValue::decimalNegOneKey};
    }

sortSpec:
    valueFieldname metaSort {
        $$ = {$1, $2};
    } | valueFieldname oneOrNegOne {
        $$ = {$1, $2};
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

// Optional argument for $convert.
onErrorArg:
    %empty {
        $$ = std::pair{KeyFieldname::onErrorArg, CNode{KeyValue::absentKey}};
    }
    | ARG_ON_ERROR expression {
        $$ = std::pair{KeyFieldname::onErrorArg, $expression};
    }
;

// Optional argument for $convert.
onNullArg:
    %empty {
        $$ = std::pair{KeyFieldname::onNullArg, CNode{KeyValue::absentKey}};
    }
    | ARG_ON_NULL expression {
        $$ = std::pair{KeyFieldname::onNullArg, $expression};
    }
;

convert:
    START_OBJECT CONVERT START_ORDERED_OBJECT ARG_INPUT expression[input] onErrorArg onNullArg
        ARG_TO expression[to] END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::convert, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::inputArg, $input},
                                         {KeyFieldname::toArg, $to},
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
