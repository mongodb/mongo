// tokenizer_test.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

TEST(Tokenizer, Empty1) {
    Tokenizer i(&languageEnglishV2, "");
    ASSERT(!i.more());
}

TEST(Tokenizer, Basic1) {
    Tokenizer i(&languageEnglishV2, "blue red green");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data.toString(), "blue");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data.toString(), "red");

    ASSERT(i.more());
    ASSERT_EQUALS(i.next().data.toString(), "green");

    ASSERT(!i.more());
}

TEST(Tokenizer, Basic2) {
    Tokenizer i(&languageEnglishV2, "blue-red");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();
    Token d = i.next();

    ASSERT_EQUALS(Token::TEXT, a.type);
    ASSERT_EQUALS(Token::DELIMITER, b.type);
    ASSERT_EQUALS(Token::TEXT, c.type);
    ASSERT_EQUALS(Token::INVALID, d.type);

    ASSERT_EQUALS("blue", a.data.toString());
    ASSERT_EQUALS("-", b.data.toString());
    ASSERT_EQUALS("red", c.data.toString());
}

TEST(Tokenizer, Basic3) {
    Tokenizer i(&languageEnglishV2, "blue -red");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();
    Token d = i.next();

    ASSERT_EQUALS(Token::TEXT, a.type);
    ASSERT_EQUALS(Token::DELIMITER, b.type);
    ASSERT_EQUALS(Token::TEXT, c.type);
    ASSERT_EQUALS(Token::INVALID, d.type);

    ASSERT_EQUALS("blue", a.data.toString());
    ASSERT_EQUALS("-", b.data.toString());
    ASSERT_EQUALS("red", c.data.toString());

    ASSERT_EQUALS(0U, a.offset);
    ASSERT_EQUALS(5U, b.offset);
    ASSERT_EQUALS(6U, c.offset);
}

TEST(Tokenizer, Quote1English) {
    Tokenizer i(&languageEnglishV2, "eliot's car");

    Token a = i.next();
    Token b = i.next();

    ASSERT_EQUALS("eliot's", a.data.toString());
    ASSERT_EQUALS("car", b.data.toString());
}

TEST(Tokenizer, Quote1French) {
    Tokenizer i(&languageFrenchV2, "eliot's car");

    Token a = i.next();
    Token b = i.next();
    Token c = i.next();

    ASSERT_EQUALS("eliot", a.data.toString());
    ASSERT_EQUALS("s", b.data.toString());
    ASSERT_EQUALS("car", c.data.toString());
}
}
}
