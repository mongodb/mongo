// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

namespace fts {

struct QueryToken {
    enum Type { WHITESPACE, DELIMITER, TEXT, INVALID };
    QueryToken(Type type, std::string_view data, unsigned offset, bool previousWhiteSpace)
        : type(type), data(data), offset(offset), previousWhiteSpace(previousWhiteSpace) {}

    bool ok() const {
        return type != INVALID;
    }

    Type type;
    std::string_view data;
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
    FTSQueryParser(const FTSQueryParser&) = delete;
    FTSQueryParser& operator=(const FTSQueryParser&) = delete;

public:
    FTSQueryParser(std::string_view str);
    bool more() const;
    QueryToken next();

private:
    QueryToken::Type getType(char c) const;
    bool skipWhitespace();

    unsigned _pos;
    bool _previousWhiteSpace;
    const std::string_view _raw;
};
}  // namespace fts
}  // namespace mongo
