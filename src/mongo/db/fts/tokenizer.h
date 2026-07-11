// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/db/fts/fts_language.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {

namespace fts {

struct Token {
    enum Type { WHITESPACE, DELIMITER, TEXT, INVALID };
    Token(Type type, std::string_view data, unsigned offset)
        : type(type), data(data), offset(offset) {}

    bool ok() const {
        return type != INVALID;
    }

    Type type;
    std::string_view data;
    unsigned offset;
};

class Tokenizer {
    Tokenizer(const Tokenizer&) = delete;
    Tokenizer& operator=(const Tokenizer&) = delete;

public:
    Tokenizer(const FTSLanguage* language, std::string_view str);

    bool more() const;
    Token next();

private:
    Token::Type _type(char c) const;
    bool _skipWhitespace();

    unsigned _pos;
    const std::string_view _raw;
    bool _english;
};
}  // namespace fts
}  // namespace mongo
