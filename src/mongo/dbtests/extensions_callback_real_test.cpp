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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString nss("unittests.extensions_callback_real_test");

//
// $text parsing tests.
//

TEST(ExtensionsCallbackReal, TextBasic) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"english\"}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getQuery(), "awesome");
    ASSERT_EQUALS(textExpr->getLanguage(), "english");
    ASSERT_EQUALS(textExpr->getCaseSensitive(), TextMatchExpressionBase::kCaseSensitiveDefault);
    ASSERT_EQUALS(textExpr->getDiacriticSensitive(),
                  TextMatchExpressionBase::kDiacriticSensitiveDefault);
}

TEST(ExtensionsCallbackReal, TextLanguageError) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $language:\"spanglish\"}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST(ExtensionsCallbackReal, TextCaseSensitiveTrue) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getCaseSensitive(), true);
}

TEST(ExtensionsCallbackReal, TextCaseSensitiveFalse) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $caseSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getCaseSensitive(), false);
}

TEST(ExtensionsCallbackReal, TextCaseSensitiveError) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text:{$search:\"awesome\", $caseSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST(ExtensionsCallbackReal, TextDiacriticSensitiveTrue) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getDiacriticSensitive(), true);
}

TEST(ExtensionsCallbackReal, TextDiacriticSensitiveFalse) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: false}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getDiacriticSensitive(), false);
}

TEST(ExtensionsCallbackReal, TextDiacriticSensitiveError) {
    OperationContextImpl txn;
    BSONObj query = fromjson("{$text:{$search:\"awesome\", $diacriticSensitive: 0}}");
    StatusWithMatchExpression result =
        ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement());

    ASSERT_NOT_OK(result.getStatus());
}

TEST(ExtensionsCallbackReal, TextDiacriticSensitiveAndCaseSensitiveTrue) {
    OperationContextImpl txn;
    BSONObj query =
        fromjson("{$text: {$search:\"awesome\", $diacriticSensitive: true, $caseSensitive: true}}");
    auto expr =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseText(query.firstElement()));

    ASSERT_EQUALS(MatchExpression::TEXT, expr->matchType());
    std::unique_ptr<TextMatchExpression> textExpr(
        static_cast<TextMatchExpression*>(expr.release()));
    ASSERT_EQUALS(textExpr->getDiacriticSensitive(), true);
    ASSERT_EQUALS(textExpr->getCaseSensitive(), true);
}

//
// $where parsing tests.
//

TEST(ExtensionsCallbackReal, WhereExpressionsWithSameScopeHaveSameBSONRepresentation) {
    OperationContextImpl txn;
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query1.firstElement()));
    BSONObjBuilder builder1;
    expr1->toBSON(&builder1);

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr2 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query2.firstElement()));
    BSONObjBuilder builder2;
    expr2->toBSON(&builder2);

    ASSERT_EQ(builder1.obj(), builder2.obj());
}

TEST(ExtensionsCallbackReal, WhereExpressionsWithDifferentScopesHaveDifferentBSONRepresentations) {
    OperationContextImpl txn;
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query1.firstElement()));
    BSONObjBuilder builder1;
    expr1->toBSON(&builder1);

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << false)));
    auto expr2 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query2.firstElement()));
    BSONObjBuilder builder2;
    expr2->toBSON(&builder2);

    ASSERT_NE(builder1.obj(), builder2.obj());
}

TEST(ExtensionsCallbackReal, WhereExpressionsWithSameScopeAreEquivalent) {
    OperationContextImpl txn;
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query1.firstElement()));

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr2 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query2.firstElement()));

    ASSERT(expr1->equivalent(expr2.get()));
    ASSERT(expr2->equivalent(expr1.get()));
}

TEST(ExtensionsCallbackReal, WhereExpressionsWithDifferentScopesAreNotEquivalent) {
    OperationContextImpl txn;
    const char code[] = "function(){ return a; }";

    BSONObj query1 = BSON("$where" << BSONCodeWScope(code, BSON("a" << true)));
    auto expr1 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query1.firstElement()));

    BSONObj query2 = BSON("$where" << BSONCodeWScope(code, BSON("a" << false)));
    auto expr2 =
        unittest::assertGet(ExtensionsCallbackReal(&txn, &nss).parseWhere(query2.firstElement()));

    ASSERT_FALSE(expr1->equivalent(expr2.get()));
    ASSERT_FALSE(expr2->equivalent(expr1.get()));
}

}  // namespace
}  // namespace mongo
