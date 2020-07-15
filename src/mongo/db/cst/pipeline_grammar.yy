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

    // Reserve pipeline stage names.
    STAGE_INHIBIT_OPTIMIZATION
    STAGE_UNION_WITH
    STAGE_SKIP
    STAGE_LIMIT

    // $unionWith arguments.
    COLL_ARG
    PIPELINE_ARG

    END_OF_FILE 0 "EOF"
;

%token <std::string> STRING
%token <int> NUMBER_INT
%token <long long> NUMBER_LONG
%token <double> NUMBER_DOUBLE
%token <bool> BOOL

//
// Semantic values (aka the C++ types produced by the actions).
//
%nterm <CNode> stageList stage inhibitOptimization unionWith num skip limit

//
// Grammar rules
//
%%

// Entry point to pipeline parsing.
pipeline: START_ARRAY stageList END_ARRAY { 
    *cst = std::move($stageList); 
};

stageList[result]:
    %empty { }
    | START_OBJECT stage END_OBJECT stageList[stagesArg] { 
        $result = CNode{CNode::ArrayChildren{$stage}};
    }
;

// Special rule to hint to the lexer that the next set of tokens should be sorted. Note that the 
// sort order is not lexicographical, but rather based on the enum generated from the %token list
// above.
START_ORDERED_OBJECT: { lexer.sortObjTokens(); } START_OBJECT;

stage:
    inhibitOptimization | unionWith | skip | limit
;

inhibitOptimization:
    STAGE_INHIBIT_OPTIMIZATION START_OBJECT END_OBJECT { 
        $inhibitOptimization =
CNode{CNode::ObjectChildren{std::pair{KeyFieldname::inhibitOptimization, CNode::noopLeaf()}}};
    };

unionWith:
    STAGE_UNION_WITH START_ORDERED_OBJECT COLL_ARG STRING PIPELINE_ARG NUMBER_DOUBLE END_OBJECT {
    auto coll = CNode{UserString($STRING)};
    auto pipeline = CNode{UserDouble($NUMBER_DOUBLE)};
    $unionWith = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::unionWith,
        CNode{CNode::ObjectChildren{
            {KeyFieldname::collArg, std::move(coll)},
            {KeyFieldname::pipelineArg, std::move(pipeline)}
     }}}}};
};

num:
    NUMBER_INT { 
        $num = CNode{UserInt($NUMBER_INT)}; 
    }
    | NUMBER_LONG { 
        $num = CNode{UserLong($NUMBER_LONG)}; 
    }
    | NUMBER_DOUBLE { 
        $num = CNode{UserDouble($NUMBER_DOUBLE)}; 
    }
;

skip:
    STAGE_SKIP num {
        $skip = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::skip, $num}}};
};

limit:
    STAGE_LIMIT num {
        $limit = CNode{CNode::ObjectChildren{std::pair{KeyFieldname::limit, $num}}};
};

%%
