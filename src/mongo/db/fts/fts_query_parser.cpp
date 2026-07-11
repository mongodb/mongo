// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_query_parser.h"

#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

namespace fts {

FTSQueryParser::FTSQueryParser(std::string_view str) : _pos(0), _raw(str) {
    skipWhitespace();
    _previousWhiteSpace = true;
}

bool FTSQueryParser::more() const {
    return _pos < _raw.size();
}

QueryToken FTSQueryParser::next() {
    if (_pos >= _raw.size())
        return QueryToken(QueryToken::INVALID, "", 0, false);

    unsigned start = _pos++;
    QueryToken::Type type = getType(_raw[start]);

    // Query Parser should never land on whitespace
    if (type == QueryToken::WHITESPACE) {
        MONGO_UNREACHABLE;
    }

    if (type == QueryToken::TEXT) {
        while (_pos < _raw.size() && getType(_raw[_pos]) == type) {
            _pos++;
        }
    }

    std::string_view ret = _raw.substr(start, _pos - start);
    bool old = _previousWhiteSpace;
    _previousWhiteSpace = skipWhitespace();

    return QueryToken(type, ret, start, old);
}

bool FTSQueryParser::skipWhitespace() {
    unsigned start = _pos;

    while (_pos < _raw.size() && getType(_raw[_pos]) == QueryToken::WHITESPACE) {
        _pos++;
    }

    return _pos > start;
}


QueryToken::Type FTSQueryParser::getType(char c) const {
    switch (c) {
        // Unicode TR29 defines these as Word Boundaries
        case '\n':  // U+000A - LF
        case '\v':  // U+000B - Veritical Tab
        case '\f':  // U+000C - Form Feed
        case '\r':  // U+000D - CR
        // Unicode TR29 remarks this could be used MidNum for Word Boundaries
        // but we treat this as a token separator
        case ' ':  // U+0020 - Space
            return QueryToken::WHITESPACE;
        // Unicode TR29 has a particular note about the complexity of hyphens.
        // Since we use them for negation, we are sensitive to them, and we simply drop
        // them otherwise from words
        case '-':
        case '"':
            return QueryToken::DELIMITER;
        default:
            return QueryToken::TEXT;
    }
}
}  // namespace fts
}  // namespace mongo
