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


#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

namespace fts {

struct QueryToken {
    enum Type { WHITESPACE, DELIMITER, TEXT, INVALID };
    QueryToken(Type type, StringData data, unsigned offset, bool previousWhiteSpace)
        : type(type), data(data), offset(offset), previousWhiteSpace(previousWhiteSpace) {}

    bool ok() const {
        return type != INVALID;
    }

    Type type;
    StringData data;
    unsigned offset;
    bool previousWhiteSpace;
};

/**
 * The pseudo EXBNF for the query parsing language is:
 *
 * SEARCH STRING = TOKEN_LIST ( ' ' TOKEN_LIST )*
 *
 * TOKEN_LIST = SEARCH_TOKEN
 *          |'-' SEARCH_TOKEN
 *          | QUOTED_SEARCH_TOKEN
 *          |'-' QUOTED_SEARCH_TOKEN
 *
 * QUOTED_SEARCH_TOKEN = '“' SEARCH_TOKEN+ '"'
 *
 * SEARCH_TOKEN = CHARACTER_EXCLUDING_SPECIAL_CHARS
 *
 * SPECIAL_CHARS = '-' | ' ' | '"'
 */
class FTSQueryParser {
    MONGO_DISALLOW_COPYING(FTSQueryParser);

public:
    FTSQueryParser(StringData str);
    bool more() const;
    QueryToken next();

private:
    QueryToken::Type getType(char c) const;
    bool skipWhitespace();

    unsigned _pos;
    bool _previousWhiteSpace;
    const StringData _raw;
};
}
}
