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

#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace fts {

namespace {
const FTSLanguage* languageEnglishV2() {
    return &FTSLanguage::make("english", TEXT_INDEX_VERSION_2);
}
const FTSLanguage* languageFrenchV2() {
    return &FTSLanguage::make("french", TEXT_INDEX_VERSION_2);
}
}  // namespace

TEST(Tokenizer, Empty1) {
    Tokenizer i(languageEnglishV2(), "");
    ASSERT(!i.more());
}

TEST(Tokenizer, Basic1) {
    Tokenizer i(languageEnglishV2(), "blue red green");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data, "blue");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data, "red");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data, "green");

    ASSERT(!i.more());
}

TEST(Tokenizer, Basic2) {
    Tokenizer i(languageEnglishV2(), "blue-red");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();
    Token d = i.next();

    ASSERT_EQUALS(Token::TEXT, a.type);
    ASSERT_EQUALS(Token::DELIMITER, b.type);
    ASSERT_EQUALS(Token::TEXT, c.type);
    ASSERT_EQUALS(Token::INVALID, d.type);

    ASSERT_EQUALS("blue", a.data);
    ASSERT_EQUALS("-", b.data);
    ASSERT_EQUALS("red", c.data);
}

TEST(Tokenizer, Basic3) {
    Tokenizer i(languageEnglishV2(), "blue -red");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();
    Token d = i.next();

    ASSERT_EQUALS(Token::TEXT, a.type);
    ASSERT_EQUALS(Token::DELIMITER, b.type);
    ASSERT_EQUALS(Token::TEXT, c.type);
    ASSERT_EQUALS(Token::INVALID, d.type);

    ASSERT_EQUALS("blue", a.data);
    ASSERT_EQUALS("-", b.data);
    ASSERT_EQUALS("red", c.data);

    ASSERT_EQUALS(0U, a.offset);
    ASSERT_EQUALS(5U, b.offset);
    ASSERT_EQUALS(6U, c.offset);
}

TEST(Tokenizer, Quote1English) {
    Tokenizer i(languageEnglishV2(), "eliot's car");

    Token a = i.next();
    Token b = i.next();

    ASSERT_EQUALS("eliot's", a.data);
    ASSERT_EQUALS("car", b.data);
}

TEST(Tokenizer, Quote1French) {
    Tokenizer i(languageFrenchV2(), "eliot's car");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();

    ASSERT_EQUALS("eliot", a.data);
    ASSERT_EQUALS("s", b.data);
    ASSERT_EQUALS("car", c.data);
}
}  // namespace fts
}  // namespace mongo
