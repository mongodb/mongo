// expression_parser_text_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text.h"

namespace mongo {

TEST(MatchExpressionParserText, Basic) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"english\"}}");

    StatusWithMatchExpression result = MatchExpressionParser::parse(query);
    ASSERT_TRUE(result.isOK());

    ASSERT_EQUALS(MatchExpression::TEXT, result.getValue()->matchType());
    std::unique_ptr<TextMatchExpression> textExp(
        static_cast<TextMatchExpression*>(result.getValue().release()));
    ASSERT_EQUALS(textExp->getQuery(), "awesome");
    ASSERT_EQUALS(textExp->getLanguage(), "english");
    ASSERT_EQUALS(textExp->getCaseSensitive(), fts::FTSQuery::caseSensitiveDefault);
}

TEST(MatchExpressionParserText, LanguageError) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"spanglish\"}}");

    StatusWithMatchExpression result = MatchExpressionParser::parse(query);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserText, CaseSensitiveTrue) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: true}}");

    StatusWithMatchExpression result = MatchExpressionParser::parse(query);
    ASSERT_TRUE(result.isOK());

    ASSERT_EQUALS(MatchExpression::TEXT, result.getValue()->matchType());
    std::unique_ptr<TextMatchExpression> textExp(
        static_cast<TextMatchExpression*>(result.getValue().release()));
    ASSERT_EQUALS(textExp->getCaseSensitive(), true);
}

TEST(MatchExpressionParserText, CaseSensitiveFalse) {
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: false}}");

    StatusWithMatchExpression result = MatchExpressionParser::parse(query);
    ASSERT_TRUE(result.isOK());

    ASSERT_EQUALS(MatchExpression::TEXT, result.getValue()->matchType());
    std::unique_ptr<TextMatchExpression> textExp(
        static_cast<TextMatchExpression*>(result.getValue().release()));
    ASSERT_EQUALS(textExp->getCaseSensitive(), false);
}

TEST(MatchExpressionParserText, CaseSensitiveError) {
    BSONObj query = fromjson("{$text:{$search:\"awesome\", $caseSensitive: 0}}");

    StatusWithMatchExpression result = MatchExpressionParser::parse(query);
    ASSERT_FALSE(result.isOK());
}
}
