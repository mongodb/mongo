/**
*    Copyright (C) 2015 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include <string>

#include "mongo/db/fts/fts_query_parser.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace fts {

FTSQueryParser::FTSQueryParser(StringData str) : _pos(0), _raw(str) {
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
        invariant(false);
    }

    if (type == QueryToken::TEXT) {
        while (_pos < _raw.size() && getType(_raw[_pos]) == type) {
            _pos++;
        }
    }

    StringData ret = _raw.substr(start, _pos - start);
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
}
}
