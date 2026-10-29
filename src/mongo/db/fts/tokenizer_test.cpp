// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
