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
    ACOS
    ACOSH
    ADD
    ALL_ELEMENTS_TRUE "allElementsTrue"
    AND
    ANY_ELEMENT_TRUE "anyElementTrue"
    ARG_CHARS "chars argument"
    ARG_COLL "coll argument"
    ARG_DATE "date argument"
    ARG_DATE_STRING "dateString argument"
    ARG_DAY "day argument"
    ARG_FILTER "filter"
    ARG_FIND "find argument"
    ARG_FORMAT "format argument"
    ARG_HOUR "hour argument"
    ARG_INPUT "input argument"
    ARG_ISO_8601 "ISO 8601 argument"
    ARG_ISO_DAY_OF_WEEK "ISO day of week argument"
    ARG_ISO_WEEK "ISO week argument"
    ARG_ISO_WEEK_YEAR "ISO week year argument"
    ARG_MILLISECOND "millisecond argument"
    ARG_MINUTE "minute argument"
    ARG_MONTH "month argument"
    ARG_ON_ERROR "onError argument"
    ARG_ON_NULL "onNull argument"
    ARG_OPTIONS "options argument"
    ARG_PIPELINE "pipeline argument"
    ARG_REGEX "regex argument"
    ARG_REPLACEMENT "replacement argument"
    ARG_SECOND "second argument"
    ARG_SIZE "size argument"
    ARG_TIMEZONE "timezone argument"
    ARG_TO "to argument"
    ASIN
    ASINH
    ATAN
    ARG_YEAR "year argument"
    ATAN2
    ATANH
    BOOL_FALSE "false"
    BOOL_TRUE "true"
    CEIL
    COMMENT
    CMP
    CONCAT
    CONST_EXPR
    CONVERT
    COS
    COSH
    DATE_FROM_PARTS
    DATE_FROM_STRING
    DATE_TO_PARTS
    DATE_TO_STRING
    DAY_OF_MONTH
    DAY_OF_WEEK
    DAY_OF_YEAR
    DECIMAL_NEGATIVE_ONE "-1 (decimal)"
    DECIMAL_ONE "1 (decimal)"
    DECIMAL_ZERO "zero (decimal)"
    DEGREES_TO_RADIANS
    DIVIDE
    DOUBLE_NEGATIVE_ONE "-1 (double)"
    DOUBLE_ONE "1 (double)"
    DOUBLE_ZERO "zero (double)"
    END_ARRAY "end of array"
    END_OBJECT "end of object"
    ELEM_MATCH "elemMatch operator"
    EQ
    EXISTS
    EXPONENT
    FLOOR
    GEO_NEAR_DISTANCE "geoNearDistance"
    GEO_NEAR_POINT "geoNearPoint"
    GT
    GTE
    HOUR
    ID
    INDEX_OF_BYTES
    INDEX_OF_CP
    INDEX_KEY "indexKey"
    INT_NEGATIVE_ONE "-1 (int)"
    INT_ONE "1 (int)"
    INT_ZERO "zero (int)"
    ISO_DAY_OF_WEEK
    ISO_WEEK
    ISO_WEEK_YEAR
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
    MILLISECOND
    MINUTE
    MOD
    MONTH
    MULTIPLY
    NE
    NOR
    NOT
    OR
    POW
    RADIANS_TO_DEGREES
    RAND_VAL "randVal"
    RECORD_ID "recordId"
    REGEX_FIND
    REGEX_FIND_ALL
    REGEX_MATCH
    REPLACE_ALL
    REPLACE_ONE
    ROUND
    RTRIM
    SEARCH_HIGHLIGHTS "searchHighlights"
    SEARCH_SCORE "searchScore"
    SECOND
    SET_DIFFERENCE "setDifference"
    SET_EQUALS "setEquals"
    SET_INTERSECTION "setIntersection"
    SET_IS_SUBSET "setIsSubset"
    SET_UNION "setUnion"
    SLICE "slice"
    SORT_KEY "sortKey"
    SIN
    SINH
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
    TAN
    TANH
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
    WEEK
    YEAR

    END_OF_FILE 0 "EOF"
;

%token <std::string> FIELDNAME "fieldname"
// If a token contians dots but is also prefixed by a dollar, it is converted to a DOTTED_FIELDNAME.
%token <std::vector<std::string>> DOTTED_FIELDNAME "fieldname containing dotted path"
%token <std::string> DOLLAR_PREF_FIELDNAME "$-prefixed fieldname"
%token <std::string> STRING "string"
%token <std::string> DOLLAR_STRING "$-prefixed string"
%token <std::string> DOLLAR_DOLLAR_STRING "$$-prefixed string"
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

//
// Semantic values (aka the C++ types produced by the actions).
//

// Possible fieldnames.
%nterm <CNode::Fieldname> aggregationProjectionFieldname projectionFieldname expressionFieldname
%nterm <CNode::Fieldname> stageAsUserFieldname argAsUserFieldname argAsProjectionPath
%nterm <CNode::Fieldname> aggExprAsUserFieldname invariableUserFieldname sortFieldname
%nterm <CNode::Fieldname> idAsUserFieldname elemMatchAsUserFieldname idAsProjectionPath
%nterm <CNode::Fieldname> valueFieldname predFieldname
%nterm <std::pair<CNode::Fieldname, CNode>> aggregationProjectField aggregationProjectionObjectField
%nterm <std::pair<CNode::Fieldname, CNode>> expressionField valueField
%nterm <std::string> arg

// Literals.
%nterm <CNode> dbPointer javascript symbol javascriptWScope int timestamp long double decimal
%nterm <CNode> minKey maxKey value string aggregationFieldPath binary undefined objectId bool date
%nterm <CNode> null regex simpleValue compoundValue valueArray valueObject valueFields variable
%nterm <CNode> typeArray typeValue

// Pipeline stages and related non-terminals.
%nterm <CNode> pipeline stageList stage inhibitOptimization unionWith skip limit project sample
%nterm <CNode> aggregationProjectFields aggregationProjectionObjectFields
%nterm <CNode> topLevelAggregationProjection aggregationProjection projectionCommon
%nterm <CNode> aggregationProjectionObject num

// Aggregate expressions.
%nterm <CNode> expression exprFixedTwoArg exprFixedThreeArg slice expressionArray expressionObject
%nterm <CNode> expressionFields maths meta add boolExprs and or not literalEscapes const literal
%nterm <CNode> stringExps concat dateFromString dateToString indexOfBytes indexOfCP ltrim regexFind
%nterm <CNode> regexFindAll regexMatch regexArgs replaceOne replaceAll rtrim split strLenBytes
%nterm <CNode> strLenCP strcasecmp substr substrBytes substrCP toLower toUpper trim
%nterm <CNode> compExprs cmp eq gt gte lt lte ne
%nterm <CNode> dateExps dateFromParts dateToParts dayOfMonth dayOfWeek dayOfYear hour
%nterm <CNode> isoDayOfWeek isoWeek isoWeekYear millisecond minute month second week year
%nterm <CNode> typeExpression convert toBool toDate toDecimal toDouble toInt toLong
%nterm <CNode> toObjectId toString type
%nterm <CNode> abs ceil divide exponent floor ln log logten mod multiply pow round sqrt subtract trunc
%nterm <std::pair<CNode::Fieldname, CNode>> onErrorArg onNullArg
%nterm <std::pair<CNode::Fieldname, CNode>> formatArg timezoneArg charsArg optionsArg
%nterm <std::pair<CNode::Fieldname, CNode>> hourArg minuteArg secondArg millisecondArg dayArg
%nterm <std::pair<CNode::Fieldname, CNode>> isoWeekArg iso8601Arg monthArg isoDayOfWeekArg
%nterm <std::vector<CNode>> expressions values exprZeroToTwo
%nterm <CNode> setExpression allElementsTrue anyElementTrue setDifference setEquals
%nterm <CNode> setIntersection setIsSubset setUnion

%nterm <CNode> trig sin cos tan sinh cosh tanh asin acos atan asinh acosh atanh atan2
%nterm <CNode> degreesToRadians radiansToDegrees
%nterm <CNode> nonArrayExpression nonArrayCompoundExpression aggregationOperator
%nterm <CNode> aggregationOperatorWithoutSlice expressionSingletonArray singleArgExpression
%nterm <CNode> nonArrayNonObjExpression
// Match expressions.
%nterm <CNode> match predicates compoundMatchExprs predValue additionalExprs
%nterm <std::pair<CNode::Fieldname, CNode>> predicate logicalExpr operatorExpression notExpr
%nterm <std::pair<CNode::Fieldname, CNode>> existsExpr typeExpr commentExpr
%nterm <CNode::Fieldname> logicalExprField
%nterm <std::vector<CNode>> typeValues

// Find Projection specific rules.
%nterm <CNode> findProject findProjectFields topLevelFindProjection findProjection
%nterm <CNode> findProjectionSlice elemMatch findProjectionObject findProjectionObjectFields
%nterm <std::pair<CNode::Fieldname, CNode>> findProjectField findProjectionObjectField

// Sort related rules.
%nterm <CNode> sortSpecs specList metaSort oneOrNegOne metaSortKeyword
%nterm <std::pair<CNode::Fieldname, CNode>> sortSpec

%start start;
// Sentinel tokens to indicate the starting point in the grammar.
%token START_PIPELINE START_MATCH START_PROJECT START_SORT

//
// Grammar rules
//
%%

start:
    START_PIPELINE pipeline {
        *cst = $pipeline;
    }
    | START_MATCH match {
        *cst = $match;
    }
    | START_PROJECT findProject {
        *cst = $findProject;
    }
    | START_SORT sortSpecs {
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
START_ORDERED_OBJECT: START_OBJECT { lexer.sortObjTokens(); };

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
    STAGE_PROJECT START_OBJECT aggregationProjectFields END_OBJECT {
        auto&& fields = $aggregationProjectFields;
        if (auto status = c_node_validation::validateNoConflictingPathsInProjectFields(fields);
            !status.isOK())
            error(@1, status.reason());
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

aggregationProjectFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | aggregationProjectFields[projectArg] aggregationProjectField {
        $$ = $projectArg;
        $$.objectChildren().emplace_back($aggregationProjectField);
    }
;

aggregationProjectField:
    ID topLevelAggregationProjection {
        $$ = {KeyFieldname::id, $topLevelAggregationProjection};
    }
    | aggregationProjectionFieldname topLevelAggregationProjection {
        $$ = {$aggregationProjectionFieldname, $topLevelAggregationProjection};
    }
;

topLevelAggregationProjection:
    aggregationProjection {
        auto projection = $1;
        $$ = stdx::holds_alternative<CNode::ObjectChildren>(projection.payload) &&
            stdx::holds_alternative<FieldnamePath>(projection.objectChildren()[0].first) ?
            c_node_disambiguation::disambiguateCompoundProjection(std::move(projection)) :
            std::move(projection);
        if (stdx::holds_alternative<CompoundInconsistentKey>($$.payload))
            // TODO SERVER-50498: error() instead of uasserting
            uasserted(ErrorCodes::FailedToParse, "object project field cannot contain both "
                                                 "inclusion and exclusion indicators");
    }
;

aggregationProjection:
    projectionCommon
    | aggregationProjectionObject
    | aggregationOperator
;

projectionCommon:
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
        $$ = CNode{NonZeroKey{Decimal128{1.0}}};
    }
    | DECIMAL_NEGATIVE_ONE {
        $$ = CNode{NonZeroKey{Decimal128{-1.0}}};
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
    | expressionArray
;

// An aggregationProjectionFieldname is a projectionFieldname that is not positional.
aggregationProjectionFieldname:
    projectionFieldname {
        $$ = $1;
        if (stdx::holds_alternative<PositionalProjectionPath>(stdx::get<FieldnamePath>($$)))
            error(@1, "positional projection forbidden in $project aggregation pipeline stage");
    }
;

// Dollar-prefixed fieldnames are illegal.
projectionFieldname:
    FIELDNAME {
        auto components = makeVector<std::string>($1);
        if (auto positional =
            c_node_validation::validateProjectionPathAsNormalOrPositional(components);
            positional.isOK()) {
            $$ = c_node_disambiguation::disambiguateProjectionPathType(std::move(components),
                                                                       positional.getValue());
        } else {
            error(@1, positional.getStatus().reason());
        }
    }
    | argAsProjectionPath
    | DOTTED_FIELDNAME {
        auto components = $1;
        if (auto positional =
            c_node_validation::validateProjectionPathAsNormalOrPositional(components);
            positional.isOK()) {
            $$ = c_node_disambiguation::disambiguateProjectionPathType(std::move(components),
                                                                       positional.getValue());
        } else {
            error(@1, positional.getStatus().reason());
        }
    }
;

// These are permitted to contain fieldnames with multiple path components such as {"a.b.c": ""}.
aggregationProjectionObject:
    START_OBJECT aggregationProjectionObjectFields END_OBJECT {
        $$ = $aggregationProjectionObjectFields;
    }
;

// Projection objects cannot be empty.
aggregationProjectionObjectFields:
    aggregationProjectionObjectField {
        $$ = CNode::noopLeaf();
        $$.objectChildren().emplace_back($aggregationProjectionObjectField);
    }
    | aggregationProjectionObjectFields[projectArg] aggregationProjectionObjectField {
        $$ = $projectArg;
        $$.objectChildren().emplace_back($aggregationProjectionObjectField);
    }
;

aggregationProjectionObjectField:
    // _id is no longer a key when we descend past the directly projected fields.
    idAsProjectionPath aggregationProjection {
        $$ = {$idAsProjectionPath, $aggregationProjection};
    }
    | aggregationProjectionFieldname aggregationProjection {
        $$ = {$aggregationProjectionFieldname, $aggregationProjection};
    }
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
    | logicalExpr
    | commentExpr
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
operatorExpression:
    notExpr | existsExpr | typeExpr
;

existsExpr:
    EXISTS value {
        $$ = std::pair{KeyFieldname::existsExpr, $value};
    }
;

typeArray:
    START_ARRAY typeValues END_ARRAY {
        $$ = CNode{$typeValues};
    }
;

typeValues:
    %empty { }
    | typeValues[ts] typeValue {
        $$ = $ts;
        $$.emplace_back($typeValue);
    }
;

typeValue:
    num | string
;

typeExpr:
    TYPE typeValue {
        auto&& type = $typeValue;
        if (auto status = c_node_validation::validateTypeOperatorArgument(type); !status.isOK()) {
          // TODO SERVER-50498: error() on the offending literal rather than the TYPE token.
          // This will require removing the offending literal indicators in the error strings provided by the validation function.
          error(@1, status.reason());
        }
        $$ = std::pair{KeyFieldname::type, std::move(type)};
    }
    | TYPE typeArray {
        auto&& types = $typeArray;
        if (auto status = c_node_validation::validateTypeOperatorArgument(types); !status.isOK()) {
          error(@1, status.reason());
        }
       $$ = std::pair{KeyFieldname::type, std::move(types)};
    }
;

commentExpr:
   COMMENT value {
      $$ = std::pair{KeyFieldname::commentExpr, $value};
   }
;

notExpr:
    NOT regex {
        $$ = std::pair{KeyFieldname::notExpr, $regex};
    }
    // $not requires an object with at least one expression. 'compoundMatchExprs' comes before
    // 'operatorExpression' to allow us to naturally emplace_back() into the CST.
    | NOT START_OBJECT compoundMatchExprs operatorExpression END_OBJECT {
        auto&& exprs = $compoundMatchExprs;
        exprs.objectChildren().emplace_back($operatorExpression);

        $$ = std::pair{KeyFieldname::notExpr, std::move(exprs)};
    }
;

// Logical expressions accept an array of objects, with at least one element. 'additionalExprs'
// comes before 'match' to allow us to naturally emplace_back() into the CST.
logicalExpr: logicalExprField START_ARRAY additionalExprs match END_ARRAY {
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
    arg {
        $$ = UserFieldname{$1};
    }
;

argAsProjectionPath:
    arg {
        auto components = makeVector<std::string>($1);
        if (auto positional =
            c_node_validation::validateProjectionPathAsNormalOrPositional(components);
            positional.isOK())
            $$ = c_node_disambiguation::disambiguateProjectionPathType(std::move(components),
                                                                       positional.getValue());
        else
            error(@1, positional.getStatus().reason());
    }
;

arg:
    // Here we need to list all keys representing args passed to operators so they can be converted
    // back to string in contexts where they're not special. It's laborious but this is the
    // perennial Bison way.
    ARG_COLL {
        $$ = "coll";
    }
    | ARG_PIPELINE {
        $$ = "pipeline";
    }
    | ARG_SIZE {
        $$ = "size";
    }
    | ARG_INPUT {
        $$ = "input";
    }
    | ARG_TO {
        $$ = "to";
    }
    | ARG_ON_ERROR {
        $$ = "onError";
    }
    | ARG_ON_NULL {
        $$ = "onNull";
    }
    | ARG_DATE_STRING {
        $$ = "dateString";
    }
    | ARG_FORMAT {
        $$ = "format";
    }
    | ARG_TIMEZONE {
        $$ = "timezone";
    }
    | ARG_DATE {
        $$ = "date";
    }
    | ARG_CHARS {
        $$ = "chars";
    }
    | ARG_REGEX {
        $$ = "regex";
    }
    | ARG_OPTIONS {
        $$ = "options";
    }
    | ARG_FIND {
        $$ = "find";
    }
    | ARG_REPLACEMENT {
        $$ = "replacement";
    }
    | ARG_HOUR {
        $$ = UserFieldname{"hour"};
    }
    | ARG_YEAR {
        $$ = UserFieldname{"year"};
    }
    | ARG_MINUTE {
        $$ = UserFieldname{"minute"};
    }
    | ARG_SECOND {
        $$ = UserFieldname{"second"};
    }
    | ARG_MILLISECOND {
        $$ = UserFieldname{"millisecond"};
    }
    | ARG_DAY {
        $$ = UserFieldname{"day"};
    }
    | ARG_ISO_DAY_OF_WEEK {
        $$ = UserFieldname{"isoDayOfWeek"};
    }
    | ARG_ISO_WEEK {
        $$ = UserFieldname{"isoWeek"};
    }
    | ARG_ISO_WEEK_YEAR {
        $$ = UserFieldname{"isoWeekYear"};
    }
    | ARG_ISO_8601 {
        $$ = UserFieldname{"iso8601"};
    }
    | ARG_MONTH {
        $$ = UserFieldname{"month"};
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
    | SLICE {
       $$ = UserFieldname{"$slice"};
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
    | DATE_FROM_PARTS {
       $$ = UserFieldname{"$dateFromParts"};
    }
    | DATE_TO_PARTS {
       $$ = UserFieldname{"$dateToParts"};
    }
    | DAY_OF_MONTH {
       $$ = UserFieldname{"$dayOfMonth"};
    }
    | DAY_OF_WEEK {
       $$ = UserFieldname{"$dayOfWeek"};
    }
    | DAY_OF_YEAR {
       $$ = UserFieldname{"$dayOfYear"};
    }
    | HOUR {
       $$ = UserFieldname{"$hour"};
    }
    | ISO_DAY_OF_WEEK {
       $$ = UserFieldname{"$isoDayOfWeek"};
    }
    | ISO_WEEK {
       $$ = UserFieldname{"$isoWeek"};
    }
    | ISO_WEEK_YEAR {
       $$ = UserFieldname{"$isoWeekYear"};
    }
    | MILLISECOND {
       $$ = UserFieldname{"$millisecond"};
    }
    | MINUTE {
       $$ = UserFieldname{"$minute"};
    }
    | MONTH {
       $$ = UserFieldname{"$month"};
    }
    | SECOND {
       $$ = UserFieldname{"$second"};
    }
    | WEEK {
       $$ = UserFieldname{"$week"};
    }
    | YEAR {
       $$ = UserFieldname{"$year"};
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
    | ALL_ELEMENTS_TRUE {
        $$ = UserFieldname{"$allElementsTrue"};
    }
    | ANY_ELEMENT_TRUE {
        $$ = UserFieldname{"$anyElementTrue"};
    }
    | SET_DIFFERENCE {
        $$ = UserFieldname{"$setDifference"};
    }
    | SET_EQUALS {
        $$ = UserFieldname{"$setEquals"};
    }
    | SET_INTERSECTION {
        $$ = UserFieldname{"$setIntersection"};
    }
    | SET_IS_SUBSET {
        $$ = UserFieldname{"$setIsSubset"};
    }
    | SET_UNION {
        $$ = UserFieldname{"$setUnion"};
    }
    | SIN {
        $$ = UserFieldname{"$sin"};
    }
    | COS {
        $$ = UserFieldname{"$cos"};
    }
    | TAN {
        $$ = UserFieldname{"$tan"};
    }
    | SINH {
        $$ = UserFieldname{"$sinh"};
    }
    | COSH {
        $$ = UserFieldname{"$cosh"};
    }
    | TANH {
        $$ = UserFieldname{"$tanh"};
    }
    | ASIN {
        $$ = UserFieldname{"$asin"};
    }
    | ACOS {
        $$ = UserFieldname{"$acos"};
    }
    | ATAN {
        $$ = UserFieldname{"$atan"};
    }
    | ASINH {
        $$ = UserFieldname{"$asinh"};
    }
    | ACOSH {
        $$ = UserFieldname{"$acosh"};
    }
    | ATANH {
        $$ = UserFieldname{"$atanh"};
    }
    | DEGREES_TO_RADIANS {
        $$ = UserFieldname{"$degreesToRadians"};
    }
    | RADIANS_TO_DEGREES {
        $$ = UserFieldname{"$radiansToDegrees"};
    }
;

// Rules for literal non-terminals.
string:
    STRING {
        $$ = CNode{UserString{$1}};
    }
    // Here we need to list all keys in value BSON positions so they can be converted back to string
    // in contexts where they're not special. It's laborious but this is the perennial Bison way.
    | GEO_NEAR_DISTANCE {
        $$ = CNode{UserString{"geoNearDistance"}};
    }
    | GEO_NEAR_POINT {
        $$ = CNode{UserString{"geoNearPoint"}};
    }
    | INDEX_KEY {
        $$ = CNode{UserString{"indexKey"}};
    }
    | RAND_VAL {
        $$ = CNode{UserString{"randVal"}};
    }
    | RECORD_ID {
        $$ = CNode{UserString{"recordId"}};
    }
    | SEARCH_HIGHLIGHTS {
        $$ = CNode{UserString{"searchHighlights"}};
    }
    | SEARCH_SCORE {
        $$ = CNode{UserString{"searchScore"}};
    }
    | SORT_KEY {
        $$ = CNode{UserString{"sortKey"}};
    }
    | TEXT_SCORE {
        $$ = CNode{UserString{"textScore"}};
    }
;

aggregationFieldPath:
    DOLLAR_STRING {
        auto str = $1;
        auto components = std::vector<std::string>{};
        auto withoutDollar = std::pair{std::next(str.begin()), str.end()};
        boost::split(components,
                     withoutDollar,
                     [](auto&& c) { return c == '.'; });
        if (auto status = c_node_validation::validateAggregationPath(components); !status.isOK())
            error(@1, status.reason());
        $$ = CNode{AggregationPath{std::move(components)}};
    }
;

variable:
    DOLLAR_DOLLAR_STRING {
        auto str = $1;
        auto components = std::vector<std::string>{};
        auto withoutDollars = std::pair{std::next(std::next(str.begin())), str.end()};
        boost::split(components,
                     withoutDollars,
                     [](auto&& c) { return c == '.'; });
        if (auto status = c_node_validation::validateVariableNameAndPathSuffix(components); !status.isOK())
            error(@1, status.reason());
        $$ = CNode{AggregationVariablePath{std::move(components)}};
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
    | aggregationFieldPath
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
    | expressions[expressionArg] expression {
        $$ = $expressionArg;
        $$.emplace_back($expression);
    }
;

expression:
    simpleValue | expressionObject | expressionArray | aggregationOperator
;

nonArrayExpression:
    simpleValue | nonArrayCompoundExpression
;

nonArrayNonObjExpression:
    simpleValue | aggregationOperator
;

nonArrayCompoundExpression:
    expressionObject | aggregationOperator
;

aggregationOperator:
    aggregationOperatorWithoutSlice | slice
;

aggregationOperatorWithoutSlice:
    maths | boolExprs | literalEscapes | compExprs | typeExpression | stringExps | setExpression
    | trig | meta | dateExps
;

// Helper rule for expressions which take exactly two expression arguments.
exprFixedTwoArg:
    START_ARRAY expression[expr1] expression[expr2] END_ARRAY {
        $$ = CNode{CNode::ArrayChildren{$expr1, $expr2}};
    }
;

// Helper rule for expressions which take exactly three expression arguments.
exprFixedThreeArg:
    START_ARRAY expression[expr1] expression[expr2] expression[expr3] END_ARRAY {
        $$ = CNode{CNode::ArrayChildren{$expr1, $expr2, $expr3}};
    }
;

slice:
    START_OBJECT SLICE exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::slice,
                                          $exprFixedTwoArg}}};
    }
    | START_OBJECT SLICE exprFixedThreeArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::slice,
                                          $exprFixedThreeArg}}};
    }
;

// These are arrays occuring in Expressions outside of $const/$literal. They may contain further
// Expressions.
expressionArray:
    START_ARRAY expressions END_ARRAY {
        $$ = CNode{$expressions};
    }
;

// Helper rule for expressions which can take as an argument an array with exactly one element.
expressionSingletonArray:
    START_ARRAY expression END_ARRAY {
        $$ = CNode{CNode::ArrayChildren{$expression}};
    }
;

singleArgExpression: nonArrayExpression | expressionSingletonArray;

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

// All fieldnames that don't indicate agg functons/operators or start with dollars.
expressionFieldname:
    invariableUserFieldname | argAsUserFieldname | idAsUserFieldname
;

idAsUserFieldname:
    ID {
        $$ = UserFieldname{"_id"};
    }
;

elemMatchAsUserFieldname:
    ELEM_MATCH {
        $$ = UserFieldname{"$elemMatch"};
    }
;

idAsProjectionPath:
    ID {
        $$ = ProjectionPath{makeVector<std::string>("_id")};
    }
;

maths:
    add | abs | ceil | divide | exponent | floor | ln | log | logten | mod | multiply | pow | round
    | sqrt | subtract | trunc
;

meta:
    START_OBJECT META GEO_NEAR_DISTANCE END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::geoNearDistance}}}};
    }
    | START_OBJECT META GEO_NEAR_POINT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::geoNearPoint}}}};
    }
    | START_OBJECT META INDEX_KEY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::indexKey}}}};
    }
    | START_OBJECT META RAND_VAL END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::randVal}}}};
    }
    | START_OBJECT META RECORD_ID END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::recordId}}}};
    }
    | START_OBJECT META SEARCH_HIGHLIGHTS END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::searchHighlights}}}};
    }
    | START_OBJECT META SEARCH_SCORE END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::searchScore}}}};
    }
    | START_OBJECT META SORT_KEY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::sortKey}}}};
    }
    | START_OBJECT META TEXT_SCORE END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::meta, CNode{KeyValue::textScore}}}};
    }

trig:
    sin | cos | tan | sinh | cosh | tanh | asin | acos | atan | atan2 | asinh | acosh | atanh
| degreesToRadians | radiansToDegrees
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
    START_OBJECT ABS singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::abs, $singleArgExpression}}};
    }
;
ceil:
    START_OBJECT CEIL singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ceil, $singleArgExpression}}};
    }
;
divide:
      START_OBJECT DIVIDE START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::divide,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
    }
;
exponent:
        START_OBJECT EXPONENT singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::exponent, $singleArgExpression}}};
    }
;
floor:
     START_OBJECT FLOOR singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::floor, $singleArgExpression}}};
    }
;
ln:
  START_OBJECT LN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::ln, $singleArgExpression}}};
 }
;
log:
   START_OBJECT LOG START_ARRAY expression[expr1] expression[expr2] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::log,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
  }
;
logten:
      START_OBJECT LOGTEN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::logten, $singleArgExpression}}};
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
      START_OBJECT SQRT singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::sqrt, $singleArgExpression}}};
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
sin:
    START_OBJECT SIN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::sin, $singleArgExpression}}};
    }
;
cos:
    START_OBJECT COS singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::cos, $singleArgExpression}}};
    }
;
tan:
    START_OBJECT TAN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::tan, $singleArgExpression}}};
    }
;
sinh:
    START_OBJECT SINH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::sinh, $singleArgExpression}}};
    }
;
cosh:
    START_OBJECT COSH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::cosh, $singleArgExpression}}};
    }
;
tanh:
    START_OBJECT TANH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::tanh, $singleArgExpression}}};
    }
;
asin:
    START_OBJECT ASIN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::asin, $singleArgExpression}}};
    }
;
acos:
    START_OBJECT ACOS singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::acos, $singleArgExpression}}};
    }
;
atan:
    START_OBJECT ATAN singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::atan, $singleArgExpression}}};
    }
;
asinh:
    START_OBJECT ASINH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::asinh, $singleArgExpression}}};
    }
;
acosh:
    START_OBJECT ACOSH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::acosh, $singleArgExpression}}};
    }
;
atanh:
    START_OBJECT ATANH singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::atanh, $singleArgExpression}}};
    }
;
degreesToRadians:
    START_OBJECT DEGREES_TO_RADIANS singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::degreesToRadians, $singleArgExpression}}};
    }
;
radiansToDegrees:
    START_OBJECT RADIANS_TO_DEGREES singleArgExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::radiansToDegrees, $singleArgExpression}}};
    }
;

boolExprs:
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

dateExps:
    dateFromParts | dateToParts | dayOfMonth | dayOfWeek | dayOfYear | hour | isoDayOfWeek | isoWeek | isoWeekYear |
      millisecond | minute | month | second | week | year
;

hourArg:
    %empty {
        $$ = std::pair{KeyFieldname::hourArg, CNode{KeyValue::absentKey}};
    }
    | ARG_HOUR expression {
        $$ = std::pair{KeyFieldname::hourArg, $expression};
    }
;

minuteArg:
    %empty {
        $$ = std::pair{KeyFieldname::minuteArg, CNode{KeyValue::absentKey}};
    }
    | ARG_MINUTE expression {
        $$ = std::pair{KeyFieldname::minuteArg, $expression};
    }
;

secondArg:
    %empty {
        $$ = std::pair{KeyFieldname::secondArg, CNode{KeyValue::absentKey}};
    }
    | ARG_SECOND expression {
        $$ = std::pair{KeyFieldname::secondArg, $expression};
    }
;

millisecondArg:
    %empty {
        $$ = std::pair{KeyFieldname::millisecondArg, CNode{KeyValue::absentKey}};
    }
    | ARG_MILLISECOND expression {
        $$ = std::pair{KeyFieldname::millisecondArg, $expression};
    }
;

dayArg:
    %empty {
        $$ = std::pair{KeyFieldname::dayArg, CNode{KeyValue::absentKey}};
    }
    | ARG_DAY expression {
        $$ = std::pair{KeyFieldname::dayArg, $expression};
    }
;

isoDayOfWeekArg:
    %empty {
        $$ = std::pair{KeyFieldname::isoDayOfWeekArg, CNode{KeyValue::absentKey}};
    }
    | ARG_ISO_DAY_OF_WEEK expression {
        $$ = std::pair{KeyFieldname::isoDayOfWeekArg, $expression};
    }
;

isoWeekArg:
    %empty {
        $$ = std::pair{KeyFieldname::isoWeekArg, CNode{KeyValue::absentKey}};
    }
    | ARG_ISO_WEEK expression {
        $$ = std::pair{KeyFieldname::isoWeekArg, $expression};
    }
;

iso8601Arg:
    %empty {
        $$ = std::pair{KeyFieldname::iso8601Arg, CNode{KeyValue::falseKey}};
    }
    | ARG_ISO_8601 bool {
        $$ = std::pair{KeyFieldname::iso8601Arg, $bool};
    }
;

monthArg:
    %empty {
        $$ = std::pair{KeyFieldname::monthArg, CNode{KeyValue::absentKey}};
    }
    | ARG_MONTH expression {
        $$ = std::pair{KeyFieldname::monthArg, $expression};
    }
;

dateFromParts:
    START_OBJECT DATE_FROM_PARTS START_ORDERED_OBJECT dayArg hourArg millisecondArg minuteArg monthArg secondArg timezoneArg ARG_YEAR expression
            END_OBJECT END_OBJECT {
            $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dateFromParts, CNode{CNode::ObjectChildren{
                                             {KeyFieldname::yearArg, $expression},
                                             $monthArg, $dayArg, $hourArg, $minuteArg, $secondArg, $millisecondArg, $timezoneArg}}}}};
    }
    | START_OBJECT DATE_FROM_PARTS START_ORDERED_OBJECT dayArg hourArg isoDayOfWeekArg isoWeekArg ARG_ISO_WEEK_YEAR expression
        millisecondArg minuteArg monthArg secondArg timezoneArg END_OBJECT END_OBJECT {
            $$ = CNode {CNode::ObjectChildren{{KeyFieldname::dateFromParts, CNode{CNode::ObjectChildren{
                                              {KeyFieldname::isoWeekYearArg, $expression},
                                              $isoWeekArg, $isoDayOfWeekArg, $hourArg, $minuteArg, $secondArg, $millisecondArg, $timezoneArg}}}}};
    }
;

dateToParts:
     START_OBJECT DATE_TO_PARTS START_ORDERED_OBJECT ARG_DATE expression iso8601Arg timezoneArg END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dateToParts, CNode{CNode::ObjectChildren{
                                         {KeyFieldname::dateArg, $expression},
                                         $timezoneArg, $iso8601Arg}}}}};
    }
;

dayOfMonth:
    START_OBJECT DAY_OF_MONTH nonArrayNonObjExpression END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfMonth, $nonArrayNonObjExpression}}};
    }
    | START_OBJECT DAY_OF_MONTH START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfMonth, CNode{CNode::ObjectChildren{
                  {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT DAY_OF_MONTH expressionSingletonArray END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfMonth, $expressionSingletonArray}}};
    }
;

dayOfWeek:
    START_OBJECT DAY_OF_WEEK nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfWeek, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT DAY_OF_WEEK START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfWeek, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT DAY_OF_WEEK expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfWeek, $expressionSingletonArray}}};
    }
;

isoDayOfWeek:
    START_OBJECT ISO_DAY_OF_WEEK nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoDayOfWeek, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT ISO_DAY_OF_WEEK START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoDayOfWeek, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT ISO_DAY_OF_WEEK expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoDayOfWeek, $expressionSingletonArray}}};
    }
;

dayOfYear:
    START_OBJECT DAY_OF_YEAR nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfYear, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT DAY_OF_YEAR START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfYear, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT DAY_OF_YEAR expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::dayOfYear, $expressionSingletonArray}}};
    }
;

hour:
    START_OBJECT HOUR nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::hour, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT HOUR START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::hour, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT HOUR expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::hour, $expressionSingletonArray}}};
    }
;

month:
    START_OBJECT MONTH nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::month, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT MONTH START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::month, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT MONTH expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::month, $expressionSingletonArray}}};
    }
;

week:
    START_OBJECT WEEK nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::week, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT WEEK START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::week, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT WEEK expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::week, $expressionSingletonArray}}};
    }
;

isoWeek:
    START_OBJECT ISO_WEEK nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeek, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT ISO_WEEK START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeek, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT ISO_WEEK expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeek, $expressionSingletonArray}}};
    }
;

isoWeekYear:
    START_OBJECT ISO_WEEK_YEAR nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeekYear, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT ISO_WEEK_YEAR START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeekYear, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT ISO_WEEK_YEAR expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::isoWeekYear, $expressionSingletonArray}}};
    }
;

year:
    START_OBJECT YEAR nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::year, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT YEAR START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::year, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT YEAR expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::year, $expressionSingletonArray}}};
    }
;

second:
    START_OBJECT SECOND nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::second, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT SECOND START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::second, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT SECOND expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::second, $expressionSingletonArray}}};
    }
;

millisecond:
    START_OBJECT MILLISECOND nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::millisecond, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT MILLISECOND START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::millisecond, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT MILLISECOND expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::millisecond, $expressionSingletonArray}}};
    }
;

minute:
    START_OBJECT MINUTE nonArrayNonObjExpression END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::minute, $nonArrayNonObjExpression}}};

    }
    | START_OBJECT MINUTE START_ORDERED_OBJECT ARG_DATE expression timezoneArg END_OBJECT END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::minute, CNode{CNode::ObjectChildren{
                {KeyFieldname::dateArg, $expression}, $timezoneArg}}}}};
    }
    | START_OBJECT MINUTE expressionSingletonArray END_OBJECT {
       $$ = CNode{CNode::ObjectChildren{{KeyFieldname::minute, $expressionSingletonArray}}};
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

sortFieldname:
    valueFieldname {
        $sortFieldname = SortPath{makeVector<std::string>(stdx::get<UserFieldname>($valueFieldname))};
    } | DOTTED_FIELDNAME {
        auto components = $DOTTED_FIELDNAME;
        if (auto status = c_node_validation::validateSortPath(components);
            !status.isOK())
            error(@DOTTED_FIELDNAME, status.reason());
        $sortFieldname = SortPath{std::move(components)};
    }
;

sortSpec:
    sortFieldname metaSort {
        $$ = {$1, $2};
    } | sortFieldname oneOrNegOne {
        $$ = {$1, $2};
    }
;

findProject:
    START_OBJECT findProjectFields END_OBJECT {
        auto&& fields = $findProjectFields;
        if (auto status = c_node_validation::validateNoConflictingPathsInProjectFields(fields);
            !status.isOK())
            error(@1, status.reason());
        if (auto inclusion = c_node_validation::validateProjectionAsInclusionOrExclusion(fields);
            inclusion.isOK())
            $$ = CNode{CNode::ObjectChildren{std::pair{inclusion.getValue() ==
                                                       c_node_validation::IsInclusion::yes ?
                                                       KeyFieldname::projectInclusion :
                                                       KeyFieldname::projectExclusion,
                                                       std::move(fields)}}};
        else
            // Pass the location of the project token to the error reporting function.
            error(@1, inclusion.getStatus().reason());
    }
;

findProjectFields:
    %empty {
        $$ = CNode::noopLeaf();
    }
    | findProjectFields[projectArg] findProjectField {
        $$ = $projectArg;
        $$.objectChildren().emplace_back($findProjectField);
    }
;

findProjectField:
    ID topLevelFindProjection {
        $$ = {KeyFieldname::id, $topLevelFindProjection};
    }
    | projectionFieldname topLevelFindProjection {
        $$ = {$projectionFieldname, $topLevelFindProjection};
    }
;

topLevelFindProjection:
    findProjection {
        auto projection = $1;
        $$ = stdx::holds_alternative<CNode::ObjectChildren>(projection.payload) &&
            stdx::holds_alternative<FieldnamePath>(projection.objectChildren()[0].first) ?
            c_node_disambiguation::disambiguateCompoundProjection(std::move(projection)) :
            std::move(projection);
        if (stdx::holds_alternative<CompoundInconsistentKey>($$.payload))
            // TODO SERVER-50498: error() instead of uasserting
            uasserted(ErrorCodes::FailedToParse, "object project field cannot contain both "
                                                 "inclusion and exclusion indicators");
    }
;

findProjection:
    projectionCommon
    | findProjectionObject
    | aggregationOperatorWithoutSlice
    | findProjectionSlice
    | elemMatch
;

elemMatch:
    START_OBJECT ELEM_MATCH match END_OBJECT {
        $$ = {CNode::ObjectChildren{{KeyFieldname::elemMatch, $match}}};
    }
;

findProjectionSlice:
    START_OBJECT SLICE num END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::slice, $num}}};
    }
    | START_OBJECT SLICE START_ARRAY num[leftNum] num[rightNum] END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::slice,
                                          CNode{CNode::ArrayChildren{$leftNum, $rightNum}}}}};
    }
;

// These are permitted to contain fieldnames with multiple path components such as {"a.b.c": ""}.
findProjectionObject:
    START_OBJECT findProjectionObjectFields END_OBJECT {
        $$ = $findProjectionObjectFields;
    }
;

// Projection objects cannot be empty.
findProjectionObjectFields:
    findProjectionObjectField {
        $$ = CNode::noopLeaf();
        $$.objectChildren().emplace_back($findProjectionObjectField);
    }
    | findProjectionObjectFields[projectArg] findProjectionObjectField {
        $$ = $projectArg;
        $$.objectChildren().emplace_back($findProjectionObjectField);
    }
;

findProjectionObjectField:
    // _id is no longer a key when we descend past the directly projected fields.
    idAsProjectionPath findProjection {
        $$ = {$idAsProjectionPath, $findProjection};
    }
    | projectionFieldname findProjection {
        $$ = {$projectionFieldname, $findProjection};
    }
;

setExpression:
    allElementsTrue | anyElementTrue | setDifference | setEquals | setIntersection | setIsSubset
    | setUnion
;

allElementsTrue:
    START_OBJECT ALL_ELEMENTS_TRUE START_ARRAY expression END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::allElementsTrue, CNode{$expression}}}};
    }
;

anyElementTrue:
    START_OBJECT ANY_ELEMENT_TRUE START_ARRAY expression END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::anyElementTrue, CNode{$expression}}}};
    }
;

setDifference:
    START_OBJECT SET_DIFFERENCE exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::setDifference,
                                          $exprFixedTwoArg}}};
    }
;

setEquals:
    START_OBJECT SET_EQUALS START_ARRAY expression[expr1] expression[expr2] expressions
        END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::setEquals,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

setIntersection:
    START_OBJECT SET_INTERSECTION START_ARRAY expression[expr1] expression[expr2] expressions
        END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::setIntersection,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
    }
;

setIsSubset:
    START_OBJECT SET_IS_SUBSET exprFixedTwoArg END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::setIsSubset,
                                          $exprFixedTwoArg}}};
    }
;

setUnion:
    START_OBJECT SET_UNION START_ARRAY expression[expr1] expression[expr2] expressions
        END_ARRAY END_OBJECT {
        $$ = CNode{CNode::ObjectChildren{{KeyFieldname::setUnion,
                                          CNode{CNode::ArrayChildren{$expr1, $expr2}}}}};
        auto&& others = $expressions;
        auto&& array = $$.objectChildren()[0].second.arrayChildren();
        array.insert(array.end(), others.begin(), others.end());
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
    | values[valuesArg] value {
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
    | elemMatchAsUserFieldname
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
