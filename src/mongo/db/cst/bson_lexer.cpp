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

#include "mongo/platform/basic.h"

#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/pipeline_parser_gen.hpp"
#include "mongo/util/string_map.h"

namespace mongo {

namespace {

// Structure to annotate reserved tokens, used to hint the lexer into behavior once the token is
// seen. There are no semantic values associated with these tokens.
struct KeywordToken {
    boost::optional<PipelineParserGen::token_type> type;
    bool traverseChild;
    bool orderedArguments;
};

// Mapping of reserved keywords to BSON token. Any key which is not included in this map is treated
// as a terminal.
const StringMap<KeywordToken> reservedKeyLookup = {
    {"$_internalInhibitOptimization",
     {
         PipelineParserGen::token::STAGE_INHIBIT_OPTIMIZATION,
         false /* traverseChild */,
         false /* orderedArguments */
     }},
};

}  // namespace

void BSONLexer::tokenize(BSONElement elem, bool includeFieldName) {
    // The rules for tokenizing element field names fall into two buckets:
    //      1. It matches a reserved keyword or operator. In this case, the traversal rules and
    //      sensitivity to ordered nested arguments is dictated by the annotation in the lookup
    //      table.
    //      2. It is not a reserved keyword and thus is treated as a STRING literal token. The
    //      traversal rules for such elements is always to traverse but ignore argument order if the
    //      value is an object.
    KeywordToken fieldNameToken = [&] {
        if (includeFieldName) {
            if (auto it = reservedKeyLookup.find(elem.fieldNameStringData());
                it != reservedKeyLookup.end()) {
                _tokens.emplace_back(*it->second.type, getNextLoc());
                return it->second;
            } else {
                _tokens.emplace_back(
                    PipelineParserGen::make_STRING(elem.fieldName(), getNextLoc()));
                return KeywordToken{PipelineParserGen::token::STRING, true, false};
            }
        }
        return KeywordToken{boost::none, true, false};
    }();

    switch (elem.type()) {
        case BSONType::Object:
            if (fieldNameToken.orderedArguments) {
                _tokens.emplace_back(PipelineParserGen::token::START_ORDERED_OBJECT, getNextLoc());
                BSONObjIteratorSorted sortedIt(elem.embeddedObject());
                while (sortedIt.more()) {
                    tokenize(sortedIt.next(), true);
                }
                _tokens.emplace_back(PipelineParserGen::token::END_ORDERED_OBJECT, getNextLoc());
            } else {
                _tokens.emplace_back(PipelineParserGen::token::START_OBJECT, getNextLoc());
                for (auto&& nestedElem : elem.embeddedObject()) {
                    tokenize(nestedElem, true);
                }
                _tokens.emplace_back(PipelineParserGen::token::END_OBJECT, getNextLoc());
            }
            break;
        case BSONType::Array:
            _tokens.emplace_back(PipelineParserGen::token::START_ARRAY, getNextLoc());
            for (auto&& nestedElem : elem.Array()) {
                // For arrays, do not tokenize the field names.
                tokenize(nestedElem, false);
            }
            _tokens.emplace_back(PipelineParserGen::token::END_ARRAY, getNextLoc());
            break;
        case BSONType::String:
            _tokens.emplace_back(PipelineParserGen::make_STRING(elem.String(), getNextLoc()));
            break;
        case NumberLong:
            _tokens.emplace_back(
                PipelineParserGen::make_NUMBER_LONG(elem.numberLong(), getNextLoc()));
            break;
        case NumberInt:
            _tokens.emplace_back(
                PipelineParserGen::make_NUMBER_INT(elem.numberInt(), getNextLoc()));
            break;
        case NumberDouble:
            _tokens.emplace_back(
                PipelineParserGen::make_NUMBER_DOUBLE(elem.numberDouble(), getNextLoc()));
            break;
        case Bool:
            _tokens.emplace_back(PipelineParserGen::make_BOOL(elem.boolean(), getNextLoc()));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

BSONLexer::BSONLexer(std::vector<BSONElement> pipeline) {
    _tokens.emplace_back(PipelineParserGen::token::START_ARRAY, getNextLoc());
    for (auto&& elem : pipeline) {
        // Don't include field names for stages of the pipeline (aka indexes of the pipeline array).
        tokenize(elem, false);
    }
    _tokens.emplace_back(PipelineParserGen::token::END_ARRAY, getNextLoc());

    // Final token must indicate EOF.
    _tokens.emplace_back(PipelineParserGen::make_END_OF_FILE(getNextLoc()));

    // Reset the position to use in yylex().
    _position = 0;
};

PipelineParserGen::symbol_type yylex(mongo::BSONLexer& lexer) {
    return lexer.getNext();
}

}  // namespace mongo
