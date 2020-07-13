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

// Mapping of reserved keywords to BSON token. Any key which is not included in this map is assumed
// to be a user field name and is treated as a terminal by the parser.
const StringMap<PipelineParserGen::token_type> reservedKeyLookup = {
    {"$_internalInhibitOptimization", PipelineParserGen::token::STAGE_INHIBIT_OPTIMIZATION},
    {"$unionWith", PipelineParserGen::token::STAGE_UNION_WITH},
    {"coll", PipelineParserGen::token::COLL_ARG},
    {"pipeline", PipelineParserGen::token::PIPELINE_ARG},
};
bool isCompound(PipelineParserGen::symbol_type token) {
    return token.type_get() == static_cast<int>(PipelineParserGen::token::START_OBJECT) ||
        token.type_get() == static_cast<int>(PipelineParserGen::token::START_ARRAY);
}

}  // namespace

void BSONLexer::sortObjTokens() {
    // A TokenElement is similar to a BSONElement, with the payload being a vector of Bison symbols
    // if the type is compound (object or array).
    using TokenElement =
        std::pair<PipelineParserGen::symbol_type, std::vector<PipelineParserGen::symbol_type>>;
    struct TokenElementCompare {
        bool operator()(const TokenElement& elem1, const TokenElement& elem2) const {
            return elem1.first.type_get() < elem2.first.type_get();
        }
    };

    auto currentPosition = _position;
    if (_tokens[currentPosition].type_get() !=
        static_cast<int>(PipelineParserGen::token::START_OBJECT)) {
        return;
    }

    std::list<TokenElement> sortedTokenPairs;
    // Increment to get to the first token after the START_OBJECT. We will sort tokens until the
    // matching END_OBJECT is found.
    currentPosition++;
    while (_tokens[currentPosition].type_get() !=
           static_cast<int>(PipelineParserGen::token::END_OBJECT)) {
        invariant(size_t(currentPosition) < _tokens.size());

        auto keyToken = _tokens[currentPosition++];

        std::vector<PipelineParserGen::symbol_type> rhsTokens;
        rhsTokens.push_back(_tokens[currentPosition]);
        if (isCompound(_tokens[currentPosition])) {
            auto braceCount = 1;
            currentPosition++;
            // Only sort the top level tokens. If we encounter a compound type, then jump to its
            // matching bracket or brace.
            while (braceCount > 0) {
                if (isCompound(_tokens[currentPosition]))
                    braceCount++;
                if (_tokens[currentPosition].type_get() ==
                        static_cast<int>(PipelineParserGen::token::END_OBJECT) ||
                    _tokens[currentPosition].type_get() ==
                        static_cast<int>(PipelineParserGen::token::END_ARRAY))
                    braceCount--;

                rhsTokens.push_back(_tokens[currentPosition++]);
            }
        } else {
            // Scalar, already added above.
            currentPosition++;
        }
        sortedTokenPairs.push_back(std::make_pair(keyToken, rhsTokens));
    }
    sortedTokenPairs.sort(TokenElementCompare());

    // _position is at the initial START_OBJECT, and currentPosition is at its matching
    // END_OBJECT. We need to flatten the sorted list of KV pairs to get the correct order of
    // tokens.
    auto replacePosition = _position + 1;
    for (auto&& [key, rhsTokens] : sortedTokenPairs) {
        _tokens[replacePosition].clear();
        _tokens[replacePosition++].move(key);
        for (auto&& token : rhsTokens) {
            _tokens[replacePosition].clear();
            _tokens[replacePosition++].move(token);
        }
    }
}

void BSONLexer::tokenize(BSONElement elem, bool includeFieldName) {
    // Skipped when we are tokenizing arrays.
    if (includeFieldName) {
        if (auto it = reservedKeyLookup.find(elem.fieldNameStringData());
            it != reservedKeyLookup.end()) {
            // Place the token expected by the parser if this is a reserved keyword.
            _tokens.emplace_back(it->second, getNextLoc());
        } else {
            // If we don't care about the keyword, the fieldname is treated as a normal string.
            _tokens.emplace_back(PipelineParserGen::make_STRING(elem.fieldName(), getNextLoc()));
        }
    }

    switch (elem.type()) {
        case BSONType::Object:
            _tokens.emplace_back(PipelineParserGen::token::START_OBJECT, getNextLoc());
            for (auto&& nestedElem : elem.embeddedObject()) {
                tokenize(nestedElem, true);
            }
            _tokens.emplace_back(PipelineParserGen::token::END_OBJECT, getNextLoc());
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
