/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/fts/tokenizer.h"

#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {

namespace fts {

Tokenizer::Tokenizer(const FTSLanguage* language, StringData str) : _pos(0), _raw(str) {
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

    StringData ret = _raw.substr(start, _pos - start);
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
