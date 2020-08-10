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

#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/cst/bson_location.h"
#include "mongo/db/cst/pipeline_parser_gen.hpp"

namespace mongo {

class BSONLexer {
public:
    BSONLexer(BSONObj obj, PipelineParserGen::token_type startingToken);
    BSONLexer(std::vector<BSONElement> pipeline, PipelineParserGen::token_type startingToken);

    /**
     * Retrieves the next token in the stream.
     */
    PipelineParserGen::symbol_type getNext() {
        return _tokens[_position++];
    }

    /**
     * Sorts the object that starts at the current position, based on the enum for each of the field
     * name tokens.
     */
    void sortObjTokens();

    /**
     * Convenience for retrieving the token at the given offset.
     */
    auto& operator[](int offset) {
        return _tokens[offset];
    }

    /**
     * Scoped struct which pushes a location prefix for subsequently generated tokens. Pops the
     * prefix off the stack upon destruction.
     */
    struct ScopedLocationTracker {
        ScopedLocationTracker(BSONLexer* lexer, BSONLocation::LocationPrefix prefix)
            : _lexer(lexer) {
            _lexer->_locationPrefixes.emplace_back(prefix);
        }

        ~ScopedLocationTracker() {
            _lexer->_locationPrefixes.pop_back();
        }

        BSONLexer* _lexer{nullptr};
    };

private:
    // Tokenizes the given BSONElement, traversing its children if necessary. If the field name
    // should not be considered, set 'includeFieldName' to false.
    void tokenize(BSONElement elem, bool includeFieldName);

    template <class LocationType, class... Args>
    void pushToken(LocationType name, Args&&... args) {
        auto token = PipelineParserGen::symbol_type(
            std::forward<Args>(args)..., BSONLocation{std::move(name), _locationPrefixes});
        _tokens.emplace_back(std::move(token));
        _position++;
    }

    // Track the position of the input, both during construction of the list of tokens as well as
    // during parse.
    unsigned int _position = 0;  // note: counter_type is only available on 3.5+

    // A set of prefix strings that describe the current location in the lexer. As we walk the input
    // BSON, this will change depending on the context that we're parsing.
    std::vector<BSONLocation::LocationPrefix> _locationPrefixes;

    std::vector<PipelineParserGen::symbol_type> _tokens;
};

// This is the entry point for retrieving the next token from the lexer, invoked from Bison's
// yyparse().
PipelineParserGen::symbol_type yylex(mongo::BSONLexer& lexer);

}  // namespace mongo
