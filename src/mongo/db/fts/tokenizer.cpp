// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/tokenizer.h"

#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>

namespace mongo {

namespace fts {

Tokenizer::Tokenizer(const FTSLanguage* language, std::string_view str) : _pos(0), _raw(str) {
    _english = (language->str() == "english");
    _skipWhitespace();
}

bool Tokenizer::more() const {
    return _pos < _raw.size();
}

Token Tokenizer::next() {
    if (_pos >= _raw.size())
        return Token(Token::INVALID, "", 0);

    unsigned start = _pos++;
    Token::Type type = _type(_raw[start]);
    if (type == Token::WHITESPACE)
        MONGO_UNREACHABLE;

    if (type == Token::TEXT)
        while (_pos < _raw.size() && _type(_raw[_pos]) == type)
            _pos++;

    std::string_view ret = _raw.substr(start, _pos - start);
    _skipWhitespace();
    return Token(type, ret, start);
}


bool Tokenizer::_skipWhitespace() {
    unsigned start = _pos;
    while (_pos < _raw.size() && _type(_raw[_pos]) == Token::WHITESPACE)
        _pos++;
    return _pos > start;
}


Token::Type Tokenizer::_type(char c) const {
    switch (c) {
        case ' ':
        case '\f':
        case '\v':
        case '\t':
        case '\r':
        case '\n':
            return Token::WHITESPACE;
        case '\'':
            if (_english)
                return Token::TEXT;
            else
                return Token::WHITESPACE;

        case '~':
        case '`':

        case '!':
        case '@':
        case '#':
        case '$':
        case '%':
        case '^':
        case '&':
        case '*':
        case '(':
        case ')':

        case '-':

        case '=':
        case '+':

        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '\\':

        case ';':
        case ':':

        case '"':

        case '<':
        case '>':

        case ',':
        case '.':

        case '/':
        case '?':

            return Token::DELIMITER;
        default:
            return Token::TEXT;
    }
}
}  // namespace fts
}  // namespace mongo
